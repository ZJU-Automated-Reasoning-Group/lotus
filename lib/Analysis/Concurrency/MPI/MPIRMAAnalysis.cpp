/**
 * @file MPIRMAAnalysis.cpp
 * @brief MPI RMA Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace mpi {

namespace {

bool rangesOverlap(int lhs_min, int lhs_max, int rhs_min, int rhs_max) {
  if (lhs_min < 0 || lhs_max < 0 || rhs_min < 0 || rhs_max < 0) {
    return true;
  }
  return !(lhs_max < rhs_min || rhs_max < lhs_min);
}

bool isLockAllOperation(const MPIOperation &op) {
  return op.rma_lock_all;
}

struct EpochKey {
  size_t participant_class_id = 0;
  WindowID window = nullptr;
  int target_rank_min = -1;
  int target_rank_max = -1;
  bool all_targets = false;

  bool operator<(const EpochKey &other) const {
    return std::tie(participant_class_id, window, target_rank_min,
                    target_rank_max, all_targets) <
           std::tie(other.participant_class_id, other.window,
                    other.target_rank_min, other.target_rank_max,
                    other.all_targets);
  }
};

std::pair<int, int> normalizedTargetRange(const MPIOperation &op) {
  if (op.target_rank >= 0) {
    return {op.target_rank, op.target_rank};
  }
  if (op.target_rank_min >= 0 || op.target_rank_max >= 0) {
    return {op.target_rank_min, op.target_rank_max};
  }
  return {-1, -1};
}

EpochKey makeEpochKey(const MPIOperation &op, bool all_targets) {
  EpochKey key;
  key.participant_class_id = op.participant_class_id;
  key.window = op.window;
  key.all_targets = all_targets;
  if (!all_targets) {
    std::tie(key.target_rank_min, key.target_rank_max) =
        normalizedTargetRange(op);
  }
  return key;
}

MPIRMASyncModel toSemanticSyncModel(MPIRMAAnalysis::SyncModel model) {
  switch (model) {
  case MPIRMAAnalysis::SyncModel::FENCE:
    return MPIRMASyncModel::Fence;
  case MPIRMAAnalysis::SyncModel::LOCK_UNLOCK:
    return MPIRMASyncModel::LockUnlock;
  case MPIRMAAnalysis::SyncModel::PSCW:
    return MPIRMASyncModel::PSCW;
  case MPIRMAAnalysis::SyncModel::NONE:
    return MPIRMASyncModel::None;
  }
  return MPIRMASyncModel::None;
}

MPIRMAEpochCompletionKind
toSemanticEpochCompletion(MPIRMAAnalysis::EpochCompletion completion) {
  switch (completion) {
  case MPIRMAAnalysis::EpochCompletion::None:
    return MPIRMAEpochCompletionKind::None;
  case MPIRMAAnalysis::EpochCompletion::LocalOnly:
    return MPIRMAEpochCompletionKind::LocalOnly;
  case MPIRMAAnalysis::EpochCompletion::RemoteGuaranteed:
    return MPIRMAEpochCompletionKind::RemoteGuaranteed;
  }
  return MPIRMAEpochCompletionKind::None;
}

MPIRMAEpochProofKind toSemanticEpochProof(MPIRMAAnalysis::EpochProof proof) {
  switch (proof) {
  case MPIRMAAnalysis::EpochProof::Must:
    return MPIRMAEpochProofKind::Must;
  case MPIRMAAnalysis::EpochProof::May:
    return MPIRMAEpochProofKind::May;
  case MPIRMAAnalysis::EpochProof::Unknown:
    return MPIRMAEpochProofKind::Unknown;
  }
  return MPIRMAEpochProofKind::Unknown;
}

bool isPSCWSyncKind(MPIRMASyncKind kind) {
  return kind == MPIRMASyncKind::PSCWPost || kind == MPIRMASyncKind::PSCWStart ||
         kind == MPIRMASyncKind::PSCWComplete || kind == MPIRMASyncKind::PSCWWait ||
         kind == MPIRMASyncKind::PSCWTest;
}

bool isRMAWriteAccess(MPIRMAAccessKind kind) {
  return kind == MPIRMAAccessKind::Put || kind == MPIRMAAccessKind::Accumulate ||
         kind == MPIRMAAccessKind::Atomic;
}

bool participantsMayOverlap(const MPIParticipantSet &lhs,
                            const MPIParticipantSet &rhs) {
  if (lhs.unknown || rhs.unknown) {
    return true;
  }
  return lhs.mayOverlap(rhs);
}

enum class GroupAliasResult { MustAlias, NoAlias, Unknown };

const Value *canonicalGroupValue(const Value *value) {
  if (!value) {
    return nullptr;
  }
  value = value->stripPointerCasts();
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    value = load->getPointerOperand()->stripPointerCasts();
  }
  if (const auto *gep = dyn_cast<GEPOperator>(value)) {
    value = gep->getPointerOperand()->stripPointerCasts();
  }
  if (const Value *underlying = getUnderlyingObject(value)) {
    value = underlying->stripPointerCasts();
  }
  return value;
}

GroupAliasResult classifyGroupAlias(GroupID lhs, GroupID rhs) {
  if (!lhs || !rhs) {
    return GroupAliasResult::Unknown;
  }
  lhs = canonicalGroupValue(lhs);
  rhs = canonicalGroupValue(rhs);
  if (lhs == rhs) {
    return GroupAliasResult::MustAlias;
  }
  const auto *lhs_arg = dyn_cast<Argument>(lhs);
  const auto *rhs_arg = dyn_cast<Argument>(rhs);
  if (lhs_arg && rhs_arg && lhs_arg->getParent() == rhs_arg->getParent() &&
      lhs_arg->getArgNo() == rhs_arg->getArgNo()) {
    return GroupAliasResult::MustAlias;
  }
  if (lhs && rhs) {
    return GroupAliasResult::NoAlias;
  }
  return GroupAliasResult::Unknown;
}

bool isBeforeInBlock(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs->getParent() != rhs->getParent()) {
    return false;
  }
  for (const Instruction &inst : *lhs->getParent()) {
    if (&inst == lhs) {
      return true;
    }
    if (&inst == rhs) {
      return false;
    }
  }
  return false;
}

bool mustHappenBefore(const Instruction *lhs, const Instruction *rhs) {
  if (!lhs || !rhs || lhs == rhs || lhs->getFunction() != rhs->getFunction()) {
    return false;
  }
  if (lhs->getParent() == rhs->getParent()) {
    return isBeforeInBlock(lhs, rhs);
  }

  Function *func = const_cast<Function *>(lhs->getFunction());
  DominatorTree DT(*func);
  if (!DT.dominates(lhs, rhs)) {
    return false;
  }

  LoopInfo LI(DT);
  return !isPotentiallyReachable(rhs, lhs, nullptr, &DT, &LI);
}

} // namespace

void MPIRMAAnalysis::annotateOperationsInMachine(
    EpochMachine &machine, const Instruction *end_inst,
    concurrency::ProofStrength proof, StringRef reason,
    bool close_epoch) const {
  for (size_t idx : machine.op_indices) {
    auto &rma_op = const_cast<RMAOperation &>(rma_operations_[idx]);
    rma_op.sync_model = machine.model;
    rma_op.sync_start = machine.start;
    rma_op.sync_end = end_inst;
    rma_op.epoch_id = machine.epoch_id;
    rma_op.lock_all = machine.state == EpochState::LockAllOpen;
    rma_op.local_completion_only = machine.local_completion_only;
    rma_op.flush_completed = machine.remote_completion_observed;
    rma_op.exposure_epoch_observed = machine.exposure_epoch_observed;
    rma_op.synchronization_proof = proof;
    if (machine.remote_completion_observed) {
      rma_op.epoch_completion =
          MPIRMAAnalysis::EpochCompletion::RemoteGuaranteed;
    } else if (machine.local_completion_only) {
      rma_op.epoch_completion = MPIRMAAnalysis::EpochCompletion::LocalOnly;
    } else {
      rma_op.epoch_completion = MPIRMAAnalysis::EpochCompletion::None;
    }
    if (proof == concurrency::ProofStrength::Must) {
      rma_op.epoch_proof = MPIRMAAnalysis::EpochProof::Must;
    } else if (proof == concurrency::ProofStrength::May) {
      rma_op.epoch_proof = MPIRMAAnalysis::EpochProof::May;
    } else {
      rma_op.epoch_proof = MPIRMAAnalysis::EpochProof::Unknown;
    }
    rma_op.relation.kind =
        machine.local_completion_only
            ? concurrency::RelationKind::LocalOnlySynchronizationCompletion
            : concurrency::RelationKind::SameSynchronizationEpoch;
    rma_op.relation.proof = proof;
    rma_op.relation.reason = reason.str();
  }

  if (close_epoch) {
    machine.state = EpochState::Idle;
    machine.model = SyncModel::NONE;
    machine.start = nullptr;
    machine.epoch_id = 0;
    machine.local_completion_only = false;
    machine.remote_completion_observed = false;
    machine.exposure_epoch_observed = false;
    machine.op_indices.clear();
  }
}

bool MPIRMAAnalysis::transitionEpochMachine(EpochMachine &machine,
                                            const MPIOperation &op,
                                            size_t next_epoch_id) const {
  switch (op.td_type) {
  case ThreadAPI::TD_MPI_WIN_FENCE:
    if (machine.state == EpochState::FenceOpen) {
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::Must,
                                  "mpi_rma_fence_epoch", true);
      return true;
    }
    if (machine.state == EpochState::Idle) {
      machine.state = EpochState::FenceOpen;
      machine.model = SyncModel::FENCE;
      machine.start = op.inst;
      machine.epoch_id = next_epoch_id;
      return true;
    }
    return false;
  case ThreadAPI::TD_MPI_WIN_LOCK:
    if (machine.state != EpochState::Idle) {
      return false;
    }
    machine.state =
        isLockAllOperation(op) ? EpochState::LockAllOpen : EpochState::LockOpen;
    machine.model = SyncModel::LOCK_UNLOCK;
    machine.start = op.inst;
    machine.epoch_id = next_epoch_id;
    return true;
  case ThreadAPI::TD_MPI_WIN_UNLOCK:
    if (machine.state == EpochState::LockOpen) {
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::Must,
                                  "mpi_rma_lock_epoch", true);
      return true;
    }
    if (machine.state == EpochState::LockAllOpen) {
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::Must,
                                  "mpi_rma_lock_all_epoch", true);
      return true;
    }
    return false;
  case ThreadAPI::TD_MPI_WIN_FLUSH:
    if (machine.state != EpochState::LockOpen &&
        machine.state != EpochState::LockAllOpen) {
      return false;
    }
    machine.remote_completion_observed =
        op.td_type != ThreadAPI::TD_MPI_WIN_FLUSH ||
        !op.rma_local_completion_only;
    machine.local_completion_only = op.rma_local_completion_only;
    annotateOperationsInMachine(
        machine, op.inst,
        op.rma_local_completion_only ? concurrency::ProofStrength::May
                                     : concurrency::ProofStrength::Must,
        op.rma_local_completion_only ? "mpi_rma_flush_local_completion"
                                     : "mpi_rma_flush_completion",
        false);
    return true;
  case ThreadAPI::TD_MPI_WIN_SYNC:
    if (machine.state != EpochState::LockOpen &&
        machine.state != EpochState::LockAllOpen) {
      return false;
    }
    machine.remote_completion_observed = false;
    machine.local_completion_only = true;
    annotateOperationsInMachine(
        machine, op.inst, concurrency::ProofStrength::Must,
        "mpi_rma_win_sync_local_completion", false);
    return true;
  case ThreadAPI::TD_MPI_WIN_START:
    if (machine.state != EpochState::Idle) {
      return false;
    }
    machine.state = EpochState::PSCWAccessOpen;
    machine.model = SyncModel::PSCW;
    machine.start = op.inst;
    machine.epoch_id = next_epoch_id;
    return true;
  case ThreadAPI::TD_MPI_WIN_COMPLETE:
    if (machine.state != EpochState::PSCWAccessOpen) {
      return false;
    }
    annotateOperationsInMachine(machine, op.inst,
                                concurrency::ProofStrength::Must,
                                "mpi_rma_pscw_access_epoch", true);
    return true;
  case ThreadAPI::TD_MPI_WIN_POST:
    if (machine.state != EpochState::Idle) {
      return false;
    }
    machine.state = EpochState::PSCWExposureOpen;
    machine.model = SyncModel::PSCW;
    machine.start = op.inst;
    machine.epoch_id = next_epoch_id;
    return true;
  case ThreadAPI::TD_MPI_WIN_WAIT:
    if (machine.state != EpochState::PSCWExposureOpen) {
      return false;
    }
    machine.exposure_epoch_observed = true;
    annotateOperationsInMachine(machine, op.inst,
                                concurrency::ProofStrength::Must,
                                "mpi_rma_pscw_exposure_epoch", true);
    return true;
  case ThreadAPI::TD_MPI_WIN_TEST:
    if (machine.state == EpochState::PSCWExposureOpen) {
      machine.exposure_epoch_observed = true;
      annotateOperationsInMachine(machine, op.inst,
                                  concurrency::ProofStrength::May,
                                  "mpi_rma_pscw_exposure_test", false);
      return true;
    }
    return false;
  default:
    return false;
  }
}

void MPIRMAAnalysis::analyzeRMA() {
  windows_.clear();
  rma_operations_.clear();
  synchronization_facts_.clear();
  model_gaps_.clear();
  invalid_epoch_transitions_.clear();
  use_after_free_windows_.clear();
  double_window_free_.clear();

  auto happensAfter = [&](const Instruction *lhs, const Instruction *rhs) {
    if (!lhs || !rhs) {
      return false;
    }
    if (lhs->getFunction() != rhs->getFunction()) {
      return false;
    }
    return mustHappenBefore(rhs, lhs);
  };

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::RMA_WINDOW && op.window &&
        op.td_type == ThreadAPI::TD_MPI_WIN_CREATE) {
      RMAWindow window;
      window.window = op.window;
      window.create_inst = op.inst;

      auto inserted = windows_.emplace(window.window, window);
      if (!inserted.second) {
        RMAWindow &tracked = inserted.first->second;
        if (!tracked.create_inst) {
          tracked.create_inst = op.inst;
        }
      }
    } else if (op.kind == MPIOpKind::RMA_WINDOW && op.window &&
               op.td_type == ThreadAPI::TD_MPI_WIN_FREE) {
      RMAWindow &tracked = windows_[op.window];
      tracked.window = op.window;
      if (tracked.free_inst) {
        double_window_free_.push_back(op.inst);
      }
      tracked.free_inst = op.inst;
    }
  }

  std::map<EpochKey, EpochMachine> epoch_machines;
  size_t next_epoch_id = 1;

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if ((op.kind == MPIOpKind::RMA_DATA || op.kind == MPIOpKind::RMA_SYNC) &&
        op.window) {
      auto win_it = windows_.find(op.window);
      if (win_it != windows_.end() && win_it->second.free_inst &&
          happensAfter(op.inst, win_it->second.free_inst)) {
        use_after_free_windows_.push_back(op.inst);
      } else if (win_it != windows_.end() && win_it->second.free_inst &&
                 op.inst && win_it->second.free_inst &&
                 op.inst->getFunction() != win_it->second.free_inst->getFunction()) {
        MPIModelGap gap;
        gap.domain = MPIModelGapDomain::RMAEpoch;
        gap.inst = op.inst;
        gap.participant_class_id = op.participant_class_id;
        gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
        gap.relation.proof = concurrency::ProofStrength::Unknown;
        gap.relation.reason = "mpi_rma_use_after_free_order_unresolved";
        gap.code = gap.relation.reason;
        gap.provenance = "rma_analysis";
        gap.detail = op.window && win_it->second.free_inst->getFunction()
                         ? win_it->second.free_inst->getFunction()->getName().str()
                         : "cross-function";
        model_gaps_.push_back(std::move(gap));
      }
    }

    if (op.kind == MPIOpKind::RMA_DATA) {
      RMAOperation rma_op;
      rma_op.inst = op.inst;
      rma_op.function = op.function;
      rma_op.window = op.window;
      rma_op.group = op.group;
      rma_op.target_rank = op.target_rank;
      rma_op.target_rank_min = op.target_rank_min;
      rma_op.target_rank_max = op.target_rank_max;
      rma_op.target_disp = op.target_disp;
      rma_op.byte_length = op.byte_length;
      rma_op.rma_epoch_kind = RMAEpochKind::Access;
      rma_op.lock_all = false;

      size_t op_index = rma_operations_.size();
      rma_operations_.push_back(rma_op);

      bool attached = false;
      auto exact_it = epoch_machines.find(makeEpochKey(op, /*all_targets=*/false));
      if (exact_it != epoch_machines.end() &&
          exact_it->second.state != EpochState::Idle) {
        exact_it->second.op_indices.push_back(op_index);
        attached = true;
      }
      if (!attached) {
        auto all_targets_it =
            epoch_machines.find(makeEpochKey(op, /*all_targets=*/true));
        if (all_targets_it != epoch_machines.end() &&
            all_targets_it->second.state != EpochState::Idle) {
          all_targets_it->second.op_indices.push_back(op_index);
        }
      }

      auto it = windows_.find(op.window);
      if (it != windows_.end()) {
        if (op.td_type == ThreadAPI::TD_MPI_PUT) {
          it->second.put_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_GET) {
          it->second.get_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_ACCUMULATE) {
          it->second.accumulate_ops.insert(op.inst);
        }
      }
    } else if (op.kind == MPIOpKind::RMA_SYNC) {
      auto it = windows_.find(op.window);
      if (it != windows_.end()) {
        if (op.td_type == ThreadAPI::TD_MPI_WIN_FENCE) {
          it->second.fence_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_LOCK) {
          it->second.lock_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_UNLOCK) {
          it->second.unlock_ops.insert(op.inst);
        } else if (op.td_type == ThreadAPI::TD_MPI_WIN_FLUSH) {
          it->second.flush_ops.insert(op.inst);
        }
      }

      const bool all_targets =
          op.td_type == ThreadAPI::TD_MPI_WIN_FENCE ||
          op.td_type == ThreadAPI::TD_MPI_WIN_START ||
          op.td_type == ThreadAPI::TD_MPI_WIN_COMPLETE ||
          op.td_type == ThreadAPI::TD_MPI_WIN_POST ||
          op.td_type == ThreadAPI::TD_MPI_WIN_WAIT ||
          op.td_type == ThreadAPI::TD_MPI_WIN_TEST ||
          op.td_type == ThreadAPI::TD_MPI_WIN_SYNC || isLockAllOperation(op) ||
          ((op.td_type == ThreadAPI::TD_MPI_WIN_UNLOCK ||
            op.td_type == ThreadAPI::TD_MPI_WIN_FLUSH) &&
           op.rma_lock_all);

      std::vector<EpochMachine *> candidate_machines;
      if (all_targets) {
        for (auto &entry : epoch_machines) {
          if (entry.first.participant_class_id != op.participant_class_id ||
              entry.first.window != op.window ||
              entry.second.state == EpochState::Idle) {
            continue;
          }
          candidate_machines.push_back(&entry.second);
        }
        if (candidate_machines.empty()) {
          candidate_machines.push_back(
              &epoch_machines[makeEpochKey(op, /*all_targets=*/true)]);
        }
      } else {
        candidate_machines.push_back(
            &epoch_machines[makeEpochKey(op, /*all_targets=*/false)]);
      }

      for (EpochMachine *machine : candidate_machines) {
        if (!machine) {
          continue;
        }
        if (transitionEpochMachine(*machine, op, next_epoch_id)) {
          if (machine->epoch_id == next_epoch_id) {
            ++next_epoch_id;
          }
          continue;
        }
        invalid_epoch_transitions_.push_back(op.inst);
        for (size_t idx : machine->op_indices) {
          rma_operations_[idx].relation.kind =
              concurrency::RelationKind::UnknownDueToModelGap;
          rma_operations_[idx].relation.proof = concurrency::ProofStrength::May;
          rma_operations_[idx].relation.reason =
              "mpi_rma_invalid_epoch_transition";
        }
        *machine = EpochMachine{};
      }
    }
  }

  for (MPIEvent &event : process_model_.getMutableSemanticEvents()) {
    if (!event.has_rma_semantics) {
      continue;
    }
    const MPIOperation &op =
        process_model_.getAllOperations()[event.operation_index];
    event.relation = op.semantic_relation;
    if (op.kind == MPIOpKind::RMA_SYNC) {
      event.rma.is_sync_operation = true;
    }
    for (const RMAOperation &rma_op : rma_operations_) {
      if (rma_op.inst != event.inst) {
        continue;
      }
      event.rma.sync_model = toSemanticSyncModel(rma_op.sync_model);
      event.rma.epoch_id = rma_op.epoch_id;
      event.rma.lock_all = rma_op.lock_all;
      event.rma.flush_completed = rma_op.flush_completed;
      event.rma.local_completion_only = rma_op.local_completion_only;
      event.rma.exposure_epoch_observed = rma_op.exposure_epoch_observed;
      event.rma.epoch_completion =
          toSemanticEpochCompletion(rma_op.epoch_completion);
      event.rma.epoch_proof = toSemanticEpochProof(rma_op.epoch_proof);
      event.rma.sync_start = rma_op.sync_start;
      event.rma.sync_end = rma_op.sync_end;
      event.relation = rma_op.relation;

      MPIOperation &mutable_op =
          process_model_.getMutableOperations()[event.operation_index];
      mutable_op.semantic_relation = rma_op.relation;
      mutable_op.synchronization_proof = rma_op.synchronization_proof;
      break;
    }
  }

  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind != MPIOpKind::RMA_DATA && op.kind != MPIOpKind::RMA_SYNC) {
      continue;
    }
    RMASynchronizationFact fact;
    fact.inst = op.inst;
    fact.window = op.window;
    fact.group = op.group;
    fact.participant_class_id = op.participant_class_id;
    fact.participants = op.participant_set;
    fact.target_rank = op.target_rank;
    fact.target_rank_min = op.target_rank_min;
    fact.target_rank_max = op.target_rank_max;
    fact.target_disp = op.target_disp;
    fact.byte_length = op.byte_length;
    fact.access_kind = op.rma_access_kind;
    fact.sync_kind = op.rma_sync_kind;
    fact.access_epoch_kind = op.kind == MPIOpKind::RMA_DATA ? RMAEpochKind::Access
                                                            : op.rma_epoch_kind;
    fact.exposure_epoch_kind =
        op.rma_sync_kind == MPIRMASyncKind::PSCWPost ||
                op.rma_sync_kind == MPIRMASyncKind::PSCWWait ||
                op.rma_sync_kind == MPIRMASyncKind::PSCWTest
            ? RMAEpochKind::Exposure
            : op.rma_epoch_kind;
    fact.relation = op.semantic_relation;
    fact.code = op.semantic_relation.reason;
    for (const RMAOperation &rma_op : rma_operations_) {
      if (rma_op.inst != op.inst) {
        continue;
      }
      fact.epoch_id = rma_op.epoch_id;
      if (rma_op.epoch_completion == EpochCompletion::LocalOnly) {
        fact.completion = MPIRMACompletionStrength::Local;
      } else if (rma_op.epoch_completion == EpochCompletion::RemoteGuaranteed) {
        fact.completion = MPIRMACompletionStrength::Remote;
      }
      fact.relation = rma_op.relation;
      fact.code = rma_op.relation.reason;
      break;
    }
    synchronization_facts_.push_back(fact);
  }

  for (RMASynchronizationFact &fact : synchronization_facts_) {
    if (!isPSCWSyncKind(fact.sync_kind) && fact.access_kind == MPIRMAAccessKind::None) {
      continue;
    }
    if (!StringRef(fact.code).startswith("mpi_rma_pscw_")) {
      continue;
    }

    bool has_complementary_scope = false;
    bool complementary_scope_group_unknown = false;
    for (const RMASynchronizationFact &other : synchronization_facts_) {
      if (&fact == &other || fact.window != other.window) {
        continue;
      }
      if (!StringRef(other.code).startswith("mpi_rma_pscw_")) {
        continue;
      }
      if (!participantsMayOverlap(fact.participants, other.participants)) {
        continue;
      }
      const bool fact_is_access = fact.access_epoch_kind == RMAEpochKind::Access ||
                                  fact.sync_kind == MPIRMASyncKind::PSCWStart ||
                                  fact.sync_kind == MPIRMASyncKind::PSCWComplete;
      const bool other_is_access =
          other.access_epoch_kind == RMAEpochKind::Access ||
          other.sync_kind == MPIRMASyncKind::PSCWStart ||
          other.sync_kind == MPIRMASyncKind::PSCWComplete;
      if (fact_is_access == other_is_access) {
        continue;
      }
      GroupAliasResult group_alias = classifyGroupAlias(fact.group, other.group);
      if (group_alias == GroupAliasResult::NoAlias) {
        continue;
      }
      if (!rangesOverlap(fact.target_rank, fact.target_rank, other.target_rank,
                         other.target_rank) &&
          !rangesOverlap(fact.target_rank_min, fact.target_rank_max,
                         other.target_rank_min, other.target_rank_max)) {
        continue;
      }
      if (group_alias == GroupAliasResult::Unknown) {
        complementary_scope_group_unknown = true;
        continue;
      }
      has_complementary_scope = true;
      break;
    }

    if (!has_complementary_scope || complementary_scope_group_unknown) {
      fact.code = "mpi_rma_pscw_group_unresolved";
      fact.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
      fact.relation.proof = complementary_scope_group_unknown
                                ? concurrency::ProofStrength::May
                                : concurrency::ProofStrength::Unknown;
      fact.relation.reason = fact.code;
      for (RMAOperation &rma_op : rma_operations_) {
        if (rma_op.inst != fact.inst) {
          continue;
        }
        rma_op.pscw_group_unresolved = true;
        rma_op.relation = fact.relation;
        rma_op.synchronization_proof = fact.relation.proof;
        rma_op.epoch_id = 0;
        rma_op.sync_model = SyncModel::NONE;
        rma_op.sync_start = nullptr;
        rma_op.sync_end = nullptr;
        rma_op.flush_completed = false;
        rma_op.local_completion_only = false;
        rma_op.exposure_epoch_observed = false;
        rma_op.epoch_completion = EpochCompletion::None;
        rma_op.epoch_proof = EpochProof::Unknown;
      }
    }
  }
}

std::vector<const Instruction *>
MPIRMAAnalysis::findInvalidEpochTransitions() const {
  return invalid_epoch_transitions_;
}

std::vector<const Instruction *>
MPIRMAAnalysis::findUseAfterFreeWindows() const {
  return use_after_free_windows_;
}

std::vector<const Instruction *> MPIRMAAnalysis::findDoubleWindowFree() const {
  return double_window_free_;
}

MPIRMAAnalysis::SyncModel
MPIRMAAnalysis::determineSyncModel(const RMAOperation &op) const {
  return op.sync_model;
}

bool MPIRMAAnalysis::areRMAOpsConflicting(const RMAOperation &op1,
                                          const RMAOperation &op2) const {
  const RMASynchronizationFact *fact1 = nullptr;
  const RMASynchronizationFact *fact2 = nullptr;
  for (const RMASynchronizationFact &fact : synchronization_facts_) {
    if (fact.inst == op1.inst && !fact1) {
      fact1 = &fact;
    }
    if (fact.inst == op2.inst && !fact2) {
      fact2 = &fact;
    }
  }
  if (op1.window != op2.window)
    return false;
  if (fact1 && fact2 &&
      !participantsMayOverlap(fact1->participants, fact2->participants)) {
    return false;
  }
  const int lhs_target = fact1 ? fact1->target_rank : op1.target_rank;
  const int rhs_target = fact2 ? fact2->target_rank : op2.target_rank;
  const int lhs_target_min = fact1 ? fact1->target_rank_min : op1.target_rank_min;
  const int lhs_target_max = fact1 ? fact1->target_rank_max : op1.target_rank_max;
  const int rhs_target_min = fact2 ? fact2->target_rank_min : op2.target_rank_min;
  const int rhs_target_max = fact2 ? fact2->target_rank_max : op2.target_rank_max;
  if (!rangesOverlap(lhs_target, lhs_target, rhs_target, rhs_target) &&
      !rangesOverlap(lhs_target_min, lhs_target_max, rhs_target_min,
                     rhs_target_max)) {
    return false;
  }
  const int64_t lhs_disp = fact1 ? fact1->target_disp : op1.target_disp;
  const int64_t rhs_disp = fact2 ? fact2->target_disp : op2.target_disp;
  const int64_t lhs_len = fact1 && fact1->byte_length > 0 ? fact1->byte_length
                                                          : (op1.byte_length > 0 ? op1.byte_length : 1);
  const int64_t rhs_len = fact2 && fact2->byte_length > 0 ? fact2->byte_length
                                                          : (op2.byte_length > 0 ? op2.byte_length : 1);
  if (lhs_disp != -1 && rhs_disp != -1) {
    int64_t end1 = lhs_disp + lhs_len;
    int64_t end2 = rhs_disp + rhs_len;
    if (!(lhs_disp < end2 && rhs_disp < end1)) {
      return false;
    }
  }

  const CallBase *CB1 = dyn_cast<CallBase>(op1.inst);
  const CallBase *CB2 = dyn_cast<CallBase>(op2.inst);
  if (!CB1 || !CB2)
    return false;

  const Function *F1 = CB1->getCalledFunction();
  const Function *F2 = CB2->getCalledFunction();
  if (!F1 || !F2)
    return false;

  ThreadAPI::TD_TYPE t1 = thread_api_->getType(F1);
  ThreadAPI::TD_TYPE t2 = thread_api_->getType(F2);
  bool op1_is_write =
      isRMAWriteAccess(fact1 ? fact1->access_kind
                             : (t1 == ThreadAPI::TD_MPI_GET ? MPIRMAAccessKind::Get
                                                            : MPIRMAAccessKind::Put));
  bool op2_is_write =
      isRMAWriteAccess(fact2 ? fact2->access_kind
                             : (t2 == ThreadAPI::TD_MPI_GET ? MPIRMAAccessKind::Get
                                                            : MPIRMAAccessKind::Put));

  if (!op1_is_write && !op2_is_write)
    return false;

  if (fact1 && fact2) {
    if (fact1->completion == MPIRMACompletionStrength::Remote &&
        fact2->completion == MPIRMACompletionStrength::Remote &&
        fact1->epoch_id != 0 && fact1->epoch_id == fact2->epoch_id) {
      return false;
    }
    if (fact1->completion == MPIRMACompletionStrength::Local ||
        fact2->completion == MPIRMACompletionStrength::Local) {
      return true;
    }
    if ((fact1->code == "mpi_rma_pscw_group_unresolved" ||
         fact2->code == "mpi_rma_pscw_group_unresolved" ||
         op1.pscw_group_unresolved || op2.pscw_group_unresolved)) {
      return true;
    }
  }

  if (op1.sync_model == SyncModel::NONE || op2.sync_model == SyncModel::NONE) {
    return true;
  }
  if (op1.sync_model != op2.sync_model) {
    return true;
  }
  if (op1.local_completion_only || op2.local_completion_only) {
    return true;
  }
  if (op1.epoch_id != 0 && op1.epoch_id == op2.epoch_id) {
    return false;
  }
  return true;
}

std::vector<MPIRMAAnalysis::RMAOperation>
MPIRMAAnalysis::findUnsynchronizedRMAOps() const {
  std::vector<RMAOperation> unsync;
  std::set<const Instruction *> synchronized_insts;
  for (const RMASynchronizationFact &fact : synchronization_facts_) {
    if (fact.access_kind == MPIRMAAccessKind::None) {
      continue;
    }
    if (fact.relation.kind == concurrency::RelationKind::SameSynchronizationEpoch ||
        fact.relation.kind ==
            concurrency::RelationKind::LocalOnlySynchronizationCompletion) {
      synchronized_insts.insert(fact.inst);
    }
  }
  for (const RMAOperation &op : rma_operations_) {
    if (!synchronized_insts.count(op.inst) || op.sync_model == SyncModel::NONE ||
        op.pscw_group_unresolved) {
      unsync.push_back(op);
    }
  }
  return unsync;
}

std::vector<
    std::pair<MPIRMAAnalysis::RMAOperation, MPIRMAAnalysis::RMAOperation>>
MPIRMAAnalysis::findRMARaces() const {
  std::vector<std::pair<RMAOperation, RMAOperation>> races;

  for (size_t i = 0; i < rma_operations_.size(); ++i) {
    for (size_t j = i + 1; j < rma_operations_.size(); ++j) {
      if (areRMAOpsConflicting(rma_operations_[i], rma_operations_[j])) {
        races.emplace_back(rma_operations_[i], rma_operations_[j]);
      }
    }
  }

  return races;
}

std::vector<WindowID> MPIRMAAnalysis::findLeakedWindows() const {
  std::vector<WindowID> leaked;
  for (const auto &pair : windows_) {
    if (!pair.second.free_inst) {
      leaked.push_back(pair.first);
    }
  }
  return leaked;
}

} // namespace mpi

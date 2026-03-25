/**
 * @file MPICollectiveAnalysis.cpp
 * @brief MPI Collective Analysis Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"

#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mpi {

namespace {

bool disablesDeterministicMPIOrdering(int provided_level) {
  return provided_level >= 2;
}

using ProtocolScopeKey = std::tuple<size_t, size_t, size_t, size_t>;
using FrontierScopeKey = std::tuple<size_t, size_t, size_t, size_t, size_t>;

ProtocolScopeKey
getProtocolScopeKey(const MPICollectiveAnalysis::CollectiveCall &call) {
  return std::make_tuple(
      call.communicator_class_id, call.communicator_subgroup_id,
      call.collective_protocol_class_id, call.protocol_sequence_id);
}

FrontierScopeKey
getFrontierScopeKey(const MPICollectiveAnalysis::CollectiveCall &call) {
  return std::make_tuple(
      call.communicator_class_id, call.communicator_subgroup_id,
      call.participant_class_id, call.collective_protocol_class_id,
      call.protocol_sequence_id);
}

const Function *getInstructionCallee(const Instruction *inst) {
  const auto *cb = dyn_cast_or_null<CallBase>(inst);
  return cb ? cb->getCalledFunction() : nullptr;
}

std::vector<const Function *> collectRootFunctions(Module &module) {
  std::vector<const Function *> roots;
  std::set<const Function *> called_functions;

  for (const Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const Instruction &inst : instructions(function)) {
      const Function *callee = getInstructionCallee(&inst);
      if (callee && !callee->isDeclaration()) {
        called_functions.insert(callee);
      }
    }
  }

  for (const Function &function : module) {
    if (!function.isDeclaration() && called_functions.count(&function) == 0) {
      roots.push_back(&function);
    }
  }
  if (roots.empty()) {
    for (const Function &function : module) {
      if (!function.isDeclaration()) {
        roots.push_back(&function);
      }
    }
  }
  return roots;
}

} // namespace

void MPICollectiveAnalysis::analyzeCollectives() {
  collective_calls_.clear();
  protocol_frontiers_.clear();
  protocol_automaton_ = MPIProtocolAutomaton();
  protocol_diagnostics_.clear();
  std::map<FrontierScopeKey, size_t> frontier_ids;
  size_t next_frontier_id = 1;
  std::map<size_t, size_t> frontier_indices;
  const MPI::MPIRankAnalysis *rank_analysis = process_model_.getRankAnalysis();
  std::unordered_map<size_t, const MPIEvent *> collective_event_by_op_index;
  for (const MPIEvent &event : process_model_.getSemanticEvents()) {
    if (event.has_collective_semantics) {
      collective_event_by_op_index[event.operation_index] = &event;
    }
  }

  std::unordered_map<const Function *, const MPIFunctionSummary *> summary_by_function;
  for (const MPIFunctionSummary &summary : process_model_.getFunctionSummaries()) {
    summary_by_function[summary.function] = &summary;
  }

  std::vector<const MPIFunctionSummary *> root_summaries;
  for (const Function *root : collectRootFunctions(
           const_cast<Module &>(process_model_.getModule()))) {
    auto it = summary_by_function.find(root);
    if (it != summary_by_function.end()) {
      root_summaries.push_back(it->second);
    }
  }
  if (root_summaries.empty()) {
    for (const auto &entry : summary_by_function) {
      root_summaries.push_back(entry.second);
    }
  }

  auto emitCollectiveCall = [&](const MPIEvent &event,
                                const Function *participant_context) {
    const MPIOperation &op =
        process_model_.getAllOperations()[event.operation_index];
    CollectiveCall call;
    call.inst = op.inst;
    call.type = op.td_type;
    call.comm = op.communicator;
    call.blocking_mode = op.blocking_mode;
    call.collective_variant = op.collective_variant;
    call.collective_shape = op.collective_shape;
    call.communicator_class_id = event.collective.scope.communicator_class_id;
    call.communicator_subgroup_id =
        event.collective.scope.communicator_subgroup_id;
    call.participant_class_id = event.collective.scope.participant_class_id;
    call.collective_protocol_class_id =
        event.collective.scope.protocol_class_id;
    call.function = participant_context ? participant_context : op.function;
    call.participants = op.participant_set;
    call.reachability = event.collective.reachability;
    call.root_rank = event.collective.root_rank;
    call.count = event.collective.count;
    call.recv_count = event.collective.recv_count;
    call.datatype = event.collective.datatype;
    call.recv_datatype = event.collective.recv_datatype;
    call.reduction_op = event.collective.reduction_op;
    call.in_place = event.collective.in_place;

    CollectiveStateKey state_key;
    state_key.communicator_class_id = call.communicator_class_id;
    state_key.communicator_subgroup_id = call.communicator_subgroup_id;
    state_key.collective_protocol_class_id = call.collective_protocol_class_id;

    size_t slot = protocol_automaton_.allocateSlot(state_key, call.function);
    MPIProtocolState &slot_state =
        protocol_automaton_.getOrCreateState(state_key, slot);
    call.sequence_index = slot;
    call.protocol_sequence_id = slot;
    call.protocol_relation.kind = concurrency::RelationKind::SameProtocolSlot;
    call.protocol_relation.proof =
        call.reachability == ProtocolReachability::AllRanks
            ? concurrency::ProofStrength::Must
            : concurrency::ProofStrength::May;
    call.protocol_relation.reason = "mpi_collective_protocol_slot";

    if (call.protocol_relation.proof == concurrency::ProofStrength::Must &&
        process_model_.hasProvidedInitThreadLevel() &&
        disablesDeterministicMPIOrdering(
            process_model_.getProvidedInitThreadLevel())) {
      call.protocol_relation.proof = concurrency::ProofStrength::May;
      call.protocol_relation.reason =
          "mpi_collective_protocol_slot_thread_downgrade";
    }

    protocol_diagnostics_["collective_slots_tracked"]++;
    if (call.reachability != ProtocolReachability::AllRanks) {
      protocol_diagnostics_["collective_partial_reachability"]++;
    }
    if (call.reachability == ProtocolReachability::SomeRanks) {
      protocol_diagnostics_["collective_rank_filtered"]++;
    } else if (call.reachability == ProtocolReachability::Unknown &&
               rank_analysis) {
      bool rank_guarded = false;
      for (const BasicBlock *pred : predecessors(call.inst->getParent())) {
        const auto *br = dyn_cast_or_null<BranchInst>(pred->getTerminator());
        if (br && br->isConditional() &&
            rank_analysis->dependsOnRank(br->getCondition())) {
          rank_guarded = true;
          break;
        }
      }
      if (rank_guarded) {
        protocol_diagnostics_["collective_rank_filtered"]++;
        protocol_diagnostics_["collective_rank_guarded_branch"]++;
      }
    }
    if (call.participants.unknown) {
      protocol_diagnostics_["collective_frontier_model_gap"]++;
    }

    MPIProtocolTransition transition;
    transition.slot_id = slot;
    transition.type = call.type;
    transition.root_rank = call.root_rank;
    transition.count = call.count;
    transition.recv_count = call.recv_count;
    transition.datatype = call.datatype;
    transition.recv_datatype = call.recv_datatype;
    transition.reduction_op = call.reduction_op;
    transition.in_place = call.in_place;

    if (!slot_state.has_expected_type) {
      slot_state.has_expected_type = true;
      slot_state.expected_type = call.type;
      slot_state.expected_root_rank = call.root_rank;
      slot_state.expected_count = call.count;
      slot_state.expected_recv_count = call.recv_count;
      slot_state.expected_datatype = call.datatype;
      slot_state.expected_recv_datatype = call.recv_datatype;
      slot_state.expected_reduction_op = call.reduction_op;
      slot_state.expected_in_place = call.in_place;
    } else if (!MPIProtocolAutomaton::isCompatible(slot_state, transition)) {
      protocol_diagnostics_["collective_automaton_type_drift"]++;
    }

    MPIOperation &mutable_op =
        process_model_.getMutableOperations()[event.operation_index];
    mutable_op.protocol_sequence_id = slot;
    mutable_op.semantic_relation = call.protocol_relation;

    const FrontierScopeKey scope_key = getFrontierScopeKey(call);
    auto frontier_it = frontier_ids.find(scope_key);
    if (frontier_it == frontier_ids.end()) {
      frontier_it = frontier_ids.emplace(scope_key, next_frontier_id++).first;
      CollectiveProtocolFrontier frontier;
      frontier.communicator_class_id = call.communicator_class_id;
      frontier.communicator_subgroup_id = call.communicator_subgroup_id;
      frontier.participant_class_id = call.participant_class_id;
      frontier.protocol_class_id = call.collective_protocol_class_id;
      frontier.frontier_id = frontier_it->second;
      frontier.frontier_position = call.protocol_sequence_id;
      frontier.participants = call.participants;
      frontier.relation.kind = concurrency::RelationKind::SameCollectiveFrontier;
      frontier.relation.proof = call.protocol_relation.proof;
      frontier.relation.reason = call.protocol_relation.reason;
      frontier_indices[frontier.frontier_id] = protocol_frontiers_.size();
      protocol_frontiers_.push_back(frontier);
    }
    auto frontier_index_it = frontier_indices.find(frontier_it->second);
    if (frontier_index_it != frontier_indices.end()) {
      CollectiveProtocolFrontier &frontier =
          protocol_frontiers_[frontier_index_it->second];
      frontier.transitions.push_back(call.inst);
      if (!call.participants.mustEqual(frontier.participants)) {
        frontier.relation.proof = concurrency::ProofStrength::May;
        frontier.diagnostics.push_back("mpi_collective_frontier_partial_participants");
      }
      if (call.protocol_relation.proof == concurrency::ProofStrength::May) {
        frontier.relation.proof = concurrency::ProofStrength::May;
      }
    }
    collective_calls_.push_back(call);
  };

  for (const MPIFunctionSummary *summary : root_summaries) {
    if (!summary) {
      continue;
    }
    const std::vector<size_t> &collective_ops =
        !summary->expanded_collective_operation_indices.empty()
            ? summary->expanded_collective_operation_indices
            : summary->expanded_operation_indices;
    for (size_t op_index : collective_ops) {
      auto event_it = collective_event_by_op_index.find(op_index);
      if (event_it == collective_event_by_op_index.end()) {
        continue;
      }
      emitCollectiveCall(*event_it->second, summary->function);
    }
    if (summary->unresolved_indirect_call_effect &&
        !summary->expanded_collective_operation_indices.empty()) {
      protocol_diagnostics_["collective_summary_unresolved"]++;
    }
  }

  if (collective_calls_.empty()) {
    for (const MPIEvent &event : process_model_.getSemanticEvents()) {
      if (event.has_collective_semantics) {
        emitCollectiveCall(event, nullptr);
      }
    }
  }

  std::unordered_map<const Instruction *, size_t> op_index_by_inst;
  for (size_t idx = 0; idx < process_model_.getAllOperations().size(); ++idx) {
    const Instruction *inst = process_model_.getAllOperations()[idx].inst;
    if (inst) {
      op_index_by_inst[inst] = idx;
    }
  }

  std::unordered_map<const Function *, MPIFunctionSummary *> mutable_summary_by_function;
  for (MPIFunctionSummary &summary : process_model_.getMutableFunctionSummaries()) {
    mutable_summary_by_function[summary.function] = &summary;
    summary.collective_call_operation_indices.clear();
    summary.entered_collective_protocol_slots.clear();
    summary.outstanding_collective_frontier_ids.clear();
    summary.unresolved_collective_summary_effect = false;
  }

  for (const CollectiveCall &call : collective_calls_) {
    auto summary_it = mutable_summary_by_function.find(call.function);
    if (summary_it == mutable_summary_by_function.end() || !call.inst) {
      continue;
    }
    auto op_it = op_index_by_inst.find(call.inst);
    if (op_it == op_index_by_inst.end()) {
      continue;
    }
    MPIFunctionSummary &summary = *summary_it->second;
    summary.collective_call_operation_indices.push_back(op_it->second);
    summary.entered_collective_protocol_slots.push_back(call.protocol_sequence_id);
    if (call.protocol_relation.proof != concurrency::ProofStrength::Must) {
      summary.unresolved_collective_summary_effect = true;
    }
  }

  for (const CollectiveProtocolFrontier &frontier : protocol_frontiers_) {
    for (const Instruction *inst : frontier.transitions) {
      auto summary_it = mutable_summary_by_function.find(inst ? inst->getFunction() : nullptr);
      if (summary_it == mutable_summary_by_function.end()) {
        continue;
      }
      MPIFunctionSummary &summary = *summary_it->second;
      if (frontier.relation.proof != concurrency::ProofStrength::Must) {
        summary.outstanding_collective_frontier_ids.push_back(frontier.frontier_id);
        summary.unresolved_collective_summary_effect = true;
      }
    }
  }

  for (MPIFunctionSummary &summary : process_model_.getMutableFunctionSummaries()) {
    auto dedup = [](std::vector<size_t> &values) {
      std::sort(values.begin(), values.end());
      values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    dedup(summary.collective_call_operation_indices);
    dedup(summary.entered_collective_protocol_slots);
    dedup(summary.outstanding_collective_frontier_ids);
    if (summary.unresolved_indirect_call_effect &&
        !summary.collective_call_operation_indices.empty()) {
      summary.unresolved_collective_summary_effect = true;
    }
  }
}

size_t MPICollectiveAnalysis::getProtocolRelationCount(
    concurrency::RelationKind kind) const {
  size_t count = 0;
  for (const CollectiveCall &call : collective_calls_) {
    if (call.protocol_relation.kind == kind) {
      ++count;
    }
  }
  return count;
}

int MPICollectiveAnalysis::getRootArgIndex(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_MPI_BCAST:
    return 3;
  case ThreadAPI::TD_MPI_REDUCE:
    return 5;
  case ThreadAPI::TD_MPI_GATHER:
  case ThreadAPI::TD_MPI_SCATTER:
    return 6;
  default:
    return -1;
  }
}

bool MPICollectiveAnalysis::areCollectivesCompatible(
    const CollectiveCall &c1, const CollectiveCall &c2) const {
  if (!c1.comm || !c2.comm) {
    return true;
  }
  if (c1.communicator_class_id != 0 && c2.communicator_class_id != 0 &&
      c1.communicator_class_id == c2.communicator_class_id) {
  } else if (!process_model_.communicatorsMayAlias(c1.comm, c2.comm)) {
    return true;
  }

  if (c1.type != c2.type)
    return false;
  if (c1.blocking_mode != MPIBlockingMode::Unknown &&
      c2.blocking_mode != MPIBlockingMode::Unknown &&
      c1.blocking_mode != c2.blocking_mode) {
    return false;
  }
  if (c1.collective_variant != MPICollectiveVariant::Unknown &&
      c2.collective_variant != MPICollectiveVariant::Unknown &&
      c1.collective_variant != c2.collective_variant) {
    return false;
  }
  if (c1.collective_shape != MPICollectiveShape::Unknown &&
      c2.collective_shape != MPICollectiveShape::Unknown &&
      c1.collective_shape != c2.collective_shape) {
    return false;
  }

  if (c1.root_rank != -1 && c2.root_rank != -1 &&
      c1.root_rank != c2.root_rank) {
    return false;
  }
  if (c1.count != -1 && c2.count != -1 && c1.count != c2.count) {
    return false;
  }
  if (c1.recv_count != -1 && c2.recv_count != -1 &&
      c1.recv_count != c2.recv_count) {
    return false;
  }
  if (c1.datatype != -1 && c2.datatype != -1 && c1.datatype != c2.datatype) {
    return false;
  }
  if (c1.recv_datatype != -1 && c2.recv_datatype != -1 &&
      c1.recv_datatype != c2.recv_datatype) {
    return false;
  }
  if (c1.reduction_op != -1 && c2.reduction_op != -1 &&
      c1.reduction_op != c2.reduction_op) {
    return false;
  }
  if (c1.in_place != c2.in_place) {
    return false;
  }

  return true;
}

std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                      MPICollectiveAnalysis::CollectiveCall>>
MPICollectiveAnalysis::findMismatchedCollectives() const {
  std::vector<std::pair<CollectiveCall, CollectiveCall>> mismatches;
  std::map<ProtocolScopeKey, std::vector<const CollectiveProtocolFrontier *>>
      frontiers_by_scope;
  for (const CollectiveProtocolFrontier &frontier : protocol_frontiers_) {
    frontiers_by_scope[std::make_tuple(frontier.communicator_class_id,
                                       frontier.communicator_subgroup_id,
                                       frontier.protocol_class_id,
                                       frontier.frontier_position)]
        .push_back(&frontier);
  }

  for (const auto &entry : frontiers_by_scope) {
    std::vector<const CollectiveCall *> calls;
    std::set<const CollectiveCall *> unique_calls;
    for (const CollectiveProtocolFrontier *frontier : entry.second) {
      for (const CollectiveCall &call : collective_calls_) {
        if (call.communicator_class_id != frontier->communicator_class_id ||
            call.communicator_subgroup_id != frontier->communicator_subgroup_id ||
            call.collective_protocol_class_id != frontier->protocol_class_id ||
            call.protocol_sequence_id != frontier->frontier_position) {
          continue;
        }
        if (unique_calls.insert(&call).second) {
          calls.push_back(&call);
        }
      }
    }
    for (size_t i = 0; i < calls.size(); ++i) {
      for (size_t j = i + 1; j < calls.size(); ++j) {
        const CollectiveCall &c1 = *calls[i];
        const CollectiveCall &c2 = *calls[j];

        if (!process_model_.communicatorsMayAlias(c1.comm, c2.comm) &&
            !(c1.communicator_class_id != 0 &&
              c1.communicator_class_id == c2.communicator_class_id)) {
          continue;
        }

        if (!areCollectivesCompatible(c1, c2)) {
          protocol_diagnostics_["collective_mismatch_pairs"]++;
          mismatches.emplace_back(c1, c2);
        }
      }
    }
  }

  return mismatches;
}

std::vector<const Instruction *>
MPICollectiveAnalysis::findConditionalCollectives() const {
  std::vector<const Instruction *> conditional;
  const MPI::MPIRankAnalysis *rank_analysis = process_model_.getRankAnalysis();

  for (const CollectiveCall &call : collective_calls_) {
    if (call.reachability == ProtocolReachability::SomeRanks) {
      protocol_diagnostics_["collective_rank_filtered"]++;
      conditional.push_back(call.inst);
      continue;
    }
    if (call.reachability == ProtocolReachability::AllRanks) {
      continue;
    }

    if (!rank_analysis) {
      conditional.push_back(call.inst);
      continue;
    }

    const BasicBlock *BB = call.inst->getParent();

    MPI::RankExpr rank = rank_analysis->getRankAtInstruction(call.inst);
    if (rank.kind == MPI::RankExpr::Concrete ||
        rank.kind == MPI::RankExpr::Range) {
      protocol_diagnostics_["collective_rank_filtered"]++;
      conditional.push_back(call.inst);
      continue;
    }

    bool rank_guarded_predecessor = false;
    for (const BasicBlock *pred : predecessors(BB)) {
      const Instruction *term = pred->getTerminator();
      const auto *br = dyn_cast_or_null<BranchInst>(term);
      if (!br || !br->isConditional()) {
        continue;
      }
      if (rank_analysis->dependsOnRank(br->getCondition())) {
        rank_guarded_predecessor = true;
        break;
      }
    }

    if (rank_guarded_predecessor) {
      protocol_diagnostics_["collective_rank_guarded_branch"]++;
      conditional.push_back(call.inst);
    }
  }

  return conditional;
}

std::vector<std::pair<MPICollectiveAnalysis::CollectiveCall,
                      MPICollectiveAnalysis::CollectiveCall>>
MPICollectiveAnalysis::findWrongRootRanks() const {
  std::vector<std::pair<CollectiveCall, CollectiveCall>> wrong_roots;
  std::map<ProtocolScopeKey, std::vector<const CollectiveProtocolFrontier *>>
      frontiers_by_scope;
  for (const CollectiveProtocolFrontier &frontier : protocol_frontiers_) {
    frontiers_by_scope[std::make_tuple(frontier.communicator_class_id,
                                       frontier.communicator_subgroup_id,
                                       frontier.protocol_class_id,
                                       frontier.frontier_position)]
        .push_back(&frontier);
  }

  for (const auto &entry : frontiers_by_scope) {
    std::vector<const CollectiveCall *> calls;
    std::set<const CollectiveCall *> unique_calls;
    for (const CollectiveProtocolFrontier *frontier : entry.second) {
      for (const CollectiveCall &call : collective_calls_) {
        if (call.root_rank < 0) {
          continue;
        }
        if (call.communicator_class_id != frontier->communicator_class_id ||
            call.communicator_subgroup_id != frontier->communicator_subgroup_id ||
            call.collective_protocol_class_id != frontier->protocol_class_id ||
            call.protocol_sequence_id != frontier->frontier_position) {
          continue;
        }
        if (unique_calls.insert(&call).second) {
          calls.push_back(&call);
        }
      }
    }
    for (size_t i = 0; i < calls.size(); ++i) {
      for (size_t j = i + 1; j < calls.size(); ++j) {
        if (calls[i]->type != calls[j]->type) {
          continue;
        }
        if (calls[i]->root_rank != calls[j]->root_rank) {
          wrong_roots.emplace_back(*calls[i], *calls[j]);
        }
      }
    }
  }

  return wrong_roots;
}

} // namespace mpi

/**
 * @file MPIProcessModel.cpp
 * @brief MPI Process Model Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIProcessModel.h"

#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mpi {

namespace {

const Value *getOperandBySignedIndex(const CallBase *cb, int index) {
  if (!cb) {
    return nullptr;
  }
  if (index == std::numeric_limits<int>::min()) {
    return nullptr;
  }
  int resolved = index;
  if (resolved < 0) {
    resolved = static_cast<int>(cb->arg_size()) + resolved;
  }
  if (resolved < 0 || resolved >= static_cast<int>(cb->arg_size())) {
    return nullptr;
  }
  return cb->getArgOperand(static_cast<unsigned>(resolved));
}

bool isMPIWildcardValue(int value) { return value == -1 || value == -2; }

bool isMPIValidRecvRankLikeValue(int value) {
  return value >= 0 || value == -1 || value == -2;
}

bool isMPIValidSendRankLikeValue(int value) { return value >= 0 || value == -2; }

bool isLikelyNullHandle(const Value *value) {
  if (!value) {
    return true;
  }
  value = value->stripPointerCasts();
  if (isa<ConstantPointerNull>(value)) {
    return true;
  }
  if (!value->hasName()) {
    return false;
  }
  StringRef name = value->getName();
  return name.contains("MPI_REQUEST_NULL") || name.contains("MPI_COMM_NULL") ||
         name.contains("MPI_WIN_NULL") || name.contains("MPI_INFO_NULL");
}

bool isLikelyMPIInPlace(const Value *value) {
  if (!value) {
    return false;
  }
  value = value->stripPointerCasts();
  return value->hasName() && value->getName().contains("MPI_IN_PLACE");
}

bool rangesOverlap(int lhs_min, int lhs_max, int rhs_min, int rhs_max) {
  if (lhs_min < 0 || lhs_max < 0 || rhs_min < 0 || rhs_max < 0) {
    return true;
  }
  return !(lhs_max < rhs_min || rhs_max < lhs_min);
}

struct CommunicatorTraceResult;

enum class CommunicatorAliasResult { MustAlias, NoAlias, Unknown };

enum class CommunicatorTraceState { Unresolved, Resolved, Ambiguous };

struct CommunicatorTraceResult {
  CommunicatorTraceState state = CommunicatorTraceState::Unresolved;
  const Value *root = nullptr;
};

CommunicatorTraceResult traceCommunicatorValue(const Value *value,
                                               const Module *module) {
  if (!value) {
    return {};
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    current = current->stripPointerCasts();
    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      continue;
    }
    if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(current)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(current)) {
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent && module) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (cb && arg->getArgNo() < cb->arg_size()) {
            worklist.push_back(cb->getArgOperand(arg->getArgNo()));
            expanded = true;
          }
        }

        for (const Function &function : *module) {
          for (const Instruction &inst : instructions(function)) {
            const auto *cb = dyn_cast<CallBase>(&inst);
            if (!cb || arg->getArgNo() >= cb->arg_size()) {
              continue;
            }
            const Value *called = cb->getCalledOperand();
            if (called && called->stripPointerCasts() == parent) {
              worklist.push_back(cb->getArgOperand(arg->getArgNo()));
              expanded = true;
            }
          }
        }
      }
      if (expanded) {
        continue;
      }
    }

    if (const Value *underlying = getUnderlyingObject(current)) {
      current = underlying->stripPointerCasts();
    }

    if (!resolved) {
      resolved = current;
    } else if (resolved != current) {
      return {CommunicatorTraceState::Ambiguous, nullptr};
    }
  }

  if (resolved) {
    return {CommunicatorTraceState::Resolved, resolved};
  }
  return {};
}

CommunicatorAliasResult classifyCommunicatorAlias(CommunicatorID lhs,
                                                  CommunicatorID rhs,
                                                  const Module *module) {
  if (!lhs || !rhs) {
    return CommunicatorAliasResult::Unknown;
  }

  const CommunicatorTraceResult lhs_trace = traceCommunicatorValue(lhs, module);
  const CommunicatorTraceResult rhs_trace = traceCommunicatorValue(rhs, module);
  if (lhs_trace.state == CommunicatorTraceState::Ambiguous ||
      rhs_trace.state == CommunicatorTraceState::Ambiguous) {
    return CommunicatorAliasResult::Unknown;
  }
  if (lhs_trace.state == CommunicatorTraceState::Resolved &&
      rhs_trace.state == CommunicatorTraceState::Resolved &&
      lhs_trace.root == rhs_trace.root) {
    return CommunicatorAliasResult::MustAlias;
  }
  if (lhs == rhs) {
    return CommunicatorAliasResult::MustAlias;
  }

  const auto *lhs_arg = dyn_cast<Argument>(lhs);
  const auto *rhs_arg = dyn_cast<Argument>(rhs);
  if (lhs_arg && rhs_arg && lhs_arg->getParent() == rhs_arg->getParent() &&
      lhs_arg->getArgNo() == rhs_arg->getArgNo()) {
    return CommunicatorAliasResult::MustAlias;
  }
  if (lhs_trace.state == CommunicatorTraceState::Resolved &&
      rhs_trace.state == CommunicatorTraceState::Resolved) {
    return CommunicatorAliasResult::NoAlias;
  }
  return CommunicatorAliasResult::Unknown;
}

bool sameCommunicatorForProof(const MPIOperation &lhs,
                              const MPIOperation &rhs,
                              const Module *module) {
  if (lhs.communicator_class_id != 0 && rhs.communicator_class_id != 0 &&
      lhs.communicator_class_id == rhs.communicator_class_id) {
    return true;
  }
  return classifyCommunicatorAlias(lhs.communicator, rhs.communicator, module) ==
         CommunicatorAliasResult::MustAlias;
}

bool getCommunicatorRankUpperBound(const MPIOperation &op,
                                   const MPI::MPIRankAnalysis *rank_analysis,
                                   int &upper_bound) {
  if (!rank_analysis || !op.communicator) {
    return false;
  }

  int min_size = 0;
  int max_size = 0;
  if (!rank_analysis->getCommunicatorSizeRange(op.communicator, min_size,
                                               max_size) ||
      max_size <= 0) {
    return false;
  }

  upper_bound = max_size - 1;
  return true;
}

bool rankValueDefinitelyOutOfBounds(int rank_value, int max_rank,
                                    bool allow_any_source) {
  if (allow_any_source) {
    if (!isMPIValidRecvRankLikeValue(rank_value)) {
      return true;
    }
  } else if (!isMPIValidSendRankLikeValue(rank_value)) {
    return true;
  }

  return rank_value >= 0 && max_rank >= 0 && rank_value > max_rank;
}

bool rankRangeDefinitelyOutOfBounds(int min_rank, int max_rank,
                                    int communicator_max_rank) {
  if (min_rank < 0 || max_rank < 0 || communicator_max_rank < 0) {
    return false;
  }
  return min_rank > communicator_max_rank;
}

const Value *canonicalMemoryBase(const Value *value) {
  if (!value) {
    return nullptr;
  }
  value = value->stripPointerCasts();
  if (const auto *gep = dyn_cast<GEPOperator>(value)) {
    value = gep->getPointerOperand()->stripPointerCasts();
  }
  if (const Value *underlying = getUnderlyingObject(value)) {
    value = underlying->stripPointerCasts();
  }
  return value;
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

bool mayDefinitionReach(const Instruction *def, const Instruction *use) {
  if (!def || !use || def->getFunction() != use->getFunction()) {
    return false;
  }
  if (def->getParent() == use->getParent()) {
    return isBeforeInBlock(def, use);
  }

  Function *func = const_cast<Function *>(def->getFunction());
  DominatorTree DT(*func);
  return DT.dominates(def->getParent(), use->getParent());
}

bool isDirectStoreToLocation(const StoreInst *store, const Value *base) {
  if (!store || !base) {
    return false;
  }
  const Value *ptr = store->getPointerOperand()->stripPointerCasts();
  if (isa<GEPOperator>(ptr)) {
    return false;
  }
  return canonicalMemoryBase(ptr) == base;
}

bool getIndexedStoreTarget(const StoreInst *store, const Value *base,
                           uint64_t &index_out) {
  if (!store || !base) {
    return false;
  }
  const Value *ptr = store->getPointerOperand();
  while (const auto *cast = dyn_cast<BitCastOperator>(ptr)) {
    ptr = cast->getOperand(0);
  }
  const auto *gep = dyn_cast<GEPOperator>(ptr);
  if (!gep || canonicalMemoryBase(gep->getPointerOperand()) != base ||
      gep->getNumIndices() == 0) {
    return false;
  }
  const auto *idx =
      dyn_cast<ConstantInt>(gep->getOperand(gep->getNumOperands() - 1));
  if (!idx) {
    return false;
  }
  index_out = idx->getZExtValue();
  return true;
}

bool readConstCallArg(const CallBase *cb, unsigned index, int &out) {
  if (!cb || index >= cb->arg_size()) {
    return false;
  }
  const auto *ci = dyn_cast<ConstantInt>(cb->getArgOperand(index));
  if (!ci) {
    return false;
  }
  out = ci->getSExtValue();
  return true;
}

MPIEventKind classifySemanticEventKind(const MPIOperation &op) {
  switch (op.kind) {
  case MPIOpKind::INIT:
  case MPIOpKind::FINALIZE:
    return MPIEventKind::Lifecycle;
  case MPIOpKind::SESSION:
    return MPIEventKind::Session;
  case MPIOpKind::SEND_BLOCKING:
  case MPIOpKind::RECV_BLOCKING:
  case MPIOpKind::PROBE_BLOCKING:
  case MPIOpKind::SEND_NONBLOCKING:
  case MPIOpKind::RECV_NONBLOCKING:
  case MPIOpKind::PROBE_NONBLOCKING:
    return MPIEventKind::PointToPoint;
  case MPIOpKind::BARRIER_BLOCKING:
  case MPIOpKind::BARRIER_NONBLOCKING:
  case MPIOpKind::COLLECTIVE_BLOCKING:
  case MPIOpKind::COLLECTIVE_NONBLOCKING:
    return MPIEventKind::Collective;
  case MPIOpKind::WAIT:
  case MPIOpKind::TEST:
  case MPIOpKind::REQUEST_MANAGEMENT:
    return MPIEventKind::Request;
  case MPIOpKind::COMM_MANAGEMENT:
  case MPIOpKind::INTERCOMM_CREATION:
    return MPIEventKind::Communicator;
  case MPIOpKind::RMA_WINDOW:
  case MPIOpKind::RMA_DATA:
  case MPIOpKind::RMA_SYNC:
    return MPIEventKind::RMA;
  case MPIOpKind::DATATYPE_CREATE:
    return MPIEventKind::Datatype;
  case MPIOpKind::UNKNOWN:
    return MPIEventKind::Unknown;
  }
  return MPIEventKind::Unknown;
}

bool isNonBlockingRequestKind(MPIOpKind kind) {
  return kind == MPIOpKind::SEND_NONBLOCKING ||
         kind == MPIOpKind::RECV_NONBLOCKING ||
         kind == MPIOpKind::BARRIER_NONBLOCKING ||
         kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
}

bool isCollectiveKind(MPIOpKind kind) {
  return kind == MPIOpKind::BARRIER_BLOCKING ||
         kind == MPIOpKind::BARRIER_NONBLOCKING ||
         kind == MPIOpKind::COLLECTIVE_BLOCKING ||
         kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
}

bool isSendOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::SEND_BLOCKING || kind == MPIOpKind::SEND_NONBLOCKING;
}

bool isRecvOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::RECV_BLOCKING || kind == MPIOpKind::RECV_NONBLOCKING;
}

bool isBlockingPointToPointKind(MPIOpKind kind) {
  return kind == MPIOpKind::SEND_BLOCKING || kind == MPIOpKind::RECV_BLOCKING;
}

bool isResolvedRequestState(MPIRequestState state) {
  return state == MPIRequestState::MustComplete ||
         state == MPIRequestState::Freed ||
         state == MPIRequestState::Canceled;
}

MPIRequestSetKind classifyRequestSetKind(const MPIOperation &op,
                                         bool persistent) {
  if (persistent) {
    return MPIRequestSetKind::Persistent;
  }
  if (op.kind == MPIOpKind::BARRIER_NONBLOCKING ||
      op.kind == MPIOpKind::COLLECTIVE_NONBLOCKING) {
    return MPIRequestSetKind::Collective;
  }
  if (op.kind == MPIOpKind::SEND_BLOCKING || op.kind == MPIOpKind::RECV_BLOCKING ||
      op.kind == MPIOpKind::SEND_NONBLOCKING ||
      op.kind == MPIOpKind::RECV_NONBLOCKING) {
    return MPIRequestSetKind::PointToPoint;
  }
  return MPIRequestSetKind::Unknown;
}

MPIRequestCompletionScopeKind completionScopeForAction(const MPIOperation &op,
                                                       const MPIEvent &event) {
  switch (event.request.action) {
  case MPIRequestActionKind::IssueNonBlocking:
  case MPIRequestActionKind::CreatePersistent:
  case MPIRequestActionKind::ActivatePersistent:
  case MPIRequestActionKind::Cancel:
  case MPIRequestActionKind::Free:
    return event.request.requests.size() <= 1
               ? MPIRequestCompletionScopeKind::Single
               : MPIRequestCompletionScopeKind::AllOfSet;
  case MPIRequestActionKind::CompleteMust:
    if (op.td_type == ThreadAPI::TD_MPI_WAIT || op.request_arity == MPIRequestArity::Single) {
      return MPIRequestCompletionScopeKind::Single;
    }
    return MPIRequestCompletionScopeKind::AllOfSet;
  case MPIRequestActionKind::Observe:
    switch (op.td_type) {
    case ThreadAPI::TD_MPI_WAITANY:
    case ThreadAPI::TD_MPI_TESTANY:
      return event.request.completed_indices.empty()
                 ? MPIRequestCompletionScopeKind::OneOfSet
                 : MPIRequestCompletionScopeKind::Single;
    case ThreadAPI::TD_MPI_WAITSOME:
    case ThreadAPI::TD_MPI_TESTSOME:
      return event.request.completed_indices.empty()
                 ? MPIRequestCompletionScopeKind::SubsetOfSet
                 : MPIRequestCompletionScopeKind::SubsetOfSet;
    case ThreadAPI::TD_MPI_TEST:
    case ThreadAPI::TD_MPI_TESTALL:
      return event.request.completion_flag_known
                 ? (event.request.requests.size() <= 1
                        ? MPIRequestCompletionScopeKind::Single
                        : MPIRequestCompletionScopeKind::AllOfSet)
                 : MPIRequestCompletionScopeKind::Unknown;
    default:
      break;
    }
    return MPIRequestCompletionScopeKind::Unknown;
  case MPIRequestActionKind::CompleteMay:
    if (event.request.requests.size() <= 1) {
      return MPIRequestCompletionScopeKind::Single;
    }
    return MPIRequestCompletionScopeKind::SubsetOfSet;
  case MPIRequestActionKind::None:
    break;
  }
  return MPIRequestCompletionScopeKind::Unknown;
}

bool isPotentialChannelPair(const MPIOperation &send, const MPIOperation &recv,
                            const Module *module) {
  if (!isSendOperationKind(send.kind) || !isRecvOperationKind(recv.kind)) {
    return false;
  }
  if (send.communicator_class_id != 0 && recv.communicator_class_id != 0 &&
      send.communicator_class_id == recv.communicator_class_id) {
    return true;
  }
  return classifyCommunicatorAlias(send.communicator, recv.communicator,
                                   module) !=
             CommunicatorAliasResult::NoAlias ||
         (!send.communicator && !recv.communicator);
}

bool isKnownWorldCommunicatorHandle(const Value *value) {
  if (!value) {
    return false;
  }
  std::deque<const Value *> worklist;
  worklist.push_back(value);
  std::set<const Value *> visited;
  while (!worklist.empty()) {
    const Value *current = worklist.back();
    worklist.pop_back();
    if (!current || !visited.insert(current).second) {
      continue;
    }
    current = current->stripPointerCasts();
    if (const auto *alias = dyn_cast<GlobalAlias>(current)) {
      worklist.push_back(alias->getAliasee());
      continue;
    }
    const auto *global = dyn_cast<GlobalValue>(current);
    if (!global) {
      continue;
    }
    StringRef name = global->getName();
    if (name.equals("MPI_COMM_WORLD") ||
        name.equals("ompi_mpi_comm_world") ||
        name.equals("__imp_MPI_COMM_WORLD")) {
      return true;
    }
  }
  return false;
}

const Value *getCommunicatorOperand(const CallBase *cb,
                                    const MPIEffect &effect) {
  if (!cb || !effect.has_descriptor) {
    return nullptr;
  }
  const MPISemanticDescriptor &descriptor = effect.descriptor;
  switch (effect.family) {
  case MPISemanticFamily::PointToPoint:
  case MPISemanticFamily::Probe:
  case MPISemanticFamily::Communicator:
    return getOperandBySignedIndex(cb, descriptor.communicator_arg);
  case MPISemanticFamily::Request:
    if (effect.type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT ||
        effect.type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT) {
      return getOperandBySignedIndex(cb, descriptor.communicator_arg);
    }
    return nullptr;
  case MPISemanticFamily::Collective: {
    int comm_index = descriptor.communicator_arg;
    if ((effect.kind == MPIOpKind::BARRIER_NONBLOCKING ||
         effect.kind == MPIOpKind::COLLECTIVE_NONBLOCKING) &&
        descriptor.collective_nonblocking_comm_arg != -1) {
      comm_index = descriptor.collective_nonblocking_comm_arg;
    }
    return getOperandBySignedIndex(cb, comm_index);
  }
  case MPISemanticFamily::RMAWindow:
  case MPISemanticFamily::RMAData:
  case MPISemanticFamily::RMASync:
    if (descriptor.communicator_arg != -1) {
      int comm_index = descriptor.communicator_arg;
      if ((effect.semantic_tag == "win-allocate" ||
           effect.semantic_tag == "win-allocate-shared") &&
          cb->arg_size() >= 6) {
        comm_index = -3;
      }
      return getOperandBySignedIndex(cb, comm_index);
    }
    return nullptr;
  default:
    return nullptr;
  }
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

MPIOpKind MPIProcessModel::classifyOperation(const Instruction *inst,
                                             ThreadAPI::TD_TYPE type) const {
  const MPISemanticDescriptor *descriptor = lookupMPISemantic(type);
  if (!descriptor) {
    return MPIOpKind::UNKNOWN;
  }

  if (descriptor->split_into_sendrecv) {
    return MPIOpKind::UNKNOWN;
  }

  if (descriptor->trait_driven_barrier_kind) {
    return thread_api_->isNonBlockingMPIBarrier(inst)
               ? MPIOpKind::BARRIER_NONBLOCKING
               : MPIOpKind::BARRIER_BLOCKING;
  }

  if (descriptor->trait_driven_collective_kind) {
    return thread_api_->isNonBlockingMPICollective(inst)
               ? MPIOpKind::COLLECTIVE_NONBLOCKING
               : MPIOpKind::COLLECTIVE_BLOCKING;
  }

  if (type == ThreadAPI::TD_MPI_COMM_CREATE && inst) {
    const Function *callee = thread_api_->getCallee(inst);
    if (callee && StringRef(thread_api_->getSemanticTag(callee))
                      .startswith("intercomm-")) {
      return MPIOpKind::INTERCOMM_CREATION;
    }
  }

  return descriptor->kind;
}

bool MPIProcessModel::tryGetConstantInt(const Value *value, int &out) const {
  const auto *ci = dyn_cast_or_null<ConstantInt>(value);
  if (!ci) {
    return false;
  }
  out = ci->getSExtValue();
  return true;
}

bool MPIProcessModel::tryGetScalarRange(const Value *value, int &min_out,
                                        int &max_out) const {
  if (!value) {
    return false;
  }
  if (const auto *ci = dyn_cast<ConstantInt>(value)) {
    min_out = ci->getSExtValue();
    max_out = min_out;
    return true;
  }
  if (rank_analysis_) {
    return rank_analysis_->tryEvaluateIntRange(value, min_out, max_out);
  }
  return false;
}

CommunicatorID
MPIProcessModel::canonicalizeCommunicator(const Value *communicator) const {
  if (!communicator) {
    return nullptr;
  }

  const CommunicatorTraceResult trace =
      traceCommunicatorValue(communicator, &module_);
  if (trace.state == CommunicatorTraceState::Ambiguous) {
    return nullptr;
  }

  const Value *canonical = trace.root;
  if (!canonical) {
    canonical = communicator->stripPointerCasts();
  }

  std::set<const Value *> visited;
  while (canonical && visited.insert(canonical).second) {
    auto it = canonical_communicators_.find(canonical);
    if (it == canonical_communicators_.end() || !it->second ||
        it->second == canonical) {
      break;
    }
    canonical = it->second->stripPointerCasts();
  }
  return canonical;
}

void MPIProcessModel::registerCommunicatorAlias(const Value *alias,
                                                const Value *root) {
  if (!alias) {
    return;
  }

  const Value *alias_key = alias->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(alias_key)) {
    alias_key = underlying->stripPointerCasts();
  }

  CommunicatorID canonical_root =
      root ? canonicalizeCommunicator(root) : alias_key;
  if (!canonical_root) {
    canonical_root = alias_key;
  }
  canonical_communicators_[alias_key] = canonical_root;
  assignCommunicatorClass(canonical_root);
}

void MPIProcessModel::recordCommunicatorCreation(
    const Value *alias, const Value *root,
    MPICommunicatorCreationKind creation_kind,
    const MPIProcessSetFact *subgroup, StringRef topology,
    bool is_intercommunicator) {
  if (!alias) {
    return;
  }

  registerCommunicatorAlias(alias, root);
  const Value *alias_key = alias->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(alias_key)) {
    alias_key = underlying->stripPointerCasts();
  }

  CommunicatorID canonical_alias = canonicalizeCommunicator(alias_key);
  CommunicatorID canonical_root = root ? canonicalizeCommunicator(root) : nullptr;
  if (canonical_alias && canonical_root && canonical_alias != canonical_root) {
    communicator_parents_[canonical_alias] = canonical_root;
  }
  if (canonical_alias) {
    communicator_creation_kinds_[canonical_alias] = creation_kind;
    if (!topology.empty()) {
      communicator_topologies_[canonical_alias] = topology.str();
    }
    if (subgroup) {
      communicator_process_sets_[alias_key] = *subgroup;
    }
    if (is_intercommunicator) {
      intercommunicators_.insert(canonical_alias);
      communicator_subgroup_token_kinds_[canonical_alias] =
          MPICommunicatorSubgroupTokenKind::Intercomm;
    }
  }
}

void MPIProcessModel::registerCommunicatorSubgroup(const Value *alias,
                                                   const Value *root,
                                                   MPICommunicatorSubgroupTokenKind token_kind,
                                                   int subgroup_token) {
  if (!alias) {
    return;
  }
  registerCommunicatorAlias(alias, root);
  const Value *alias_key = alias->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(alias_key)) {
    alias_key = underlying->stripPointerCasts();
  }
  CommunicatorID canonical_root =
      root ? canonicalizeCommunicator(root) : alias_key;
  size_t subgroup_id = 0;
  if (token_kind == MPICommunicatorSubgroupTokenKind::SplitColorConst &&
      subgroup_token >= 0) {
    const std::string subgroup_key =
        std::to_string(assignCommunicatorClass(canonical_root)) + ":" +
        std::to_string(subgroup_token);
    subgroup_id = std::hash<std::string>{}(subgroup_key) + 1;
  } else {
    subgroup_id = next_communicator_subgroup_id_++;
  }
  communicator_subgroup_ids_[alias_key] = subgroup_id;
  communicator_subgroup_token_kinds_[alias_key] = token_kind;
}

size_t MPIProcessModel::assignCommunicatorClass(CommunicatorID canonical) {
  if (!canonical) {
    return 0;
  }
  canonical = canonical->stripPointerCasts();

  auto it = communicator_class_ids_.find(canonical);
  if (it != communicator_class_ids_.end()) {
    return it->second;
  }
  size_t id = next_communicator_class_id_++;
  communicator_class_ids_[canonical] = id;
  return id;
}

size_t
MPIProcessModel::getCommunicatorClassID(CommunicatorID communicator) const {
  CommunicatorID canonical = canonicalizeCommunicator(communicator);
  if (!canonical) {
    return 0;
  }
  auto it = communicator_class_ids_.find(canonical);
  return it != communicator_class_ids_.end() ? it->second : 0;
}

bool MPIProcessModel::communicatorsMayAlias(CommunicatorID lhs,
                                            CommunicatorID rhs) const {
  if (!lhs || !rhs) {
    return false;
  }

  const size_t lhs_class = getCommunicatorClassID(lhs);
  const size_t rhs_class = getCommunicatorClassID(rhs);
  if (lhs_class != 0 && rhs_class != 0) {
    return lhs_class == rhs_class;
  }

  return classifyCommunicatorAlias(lhs, rhs, &module_) !=
         CommunicatorAliasResult::NoAlias;
}

bool MPIProcessModel::communicatorsMustAlias(CommunicatorID lhs,
                                             CommunicatorID rhs) const {
  if (!lhs || !rhs) {
    return false;
  }

  const size_t lhs_class = getCommunicatorClassID(lhs);
  const size_t rhs_class = getCommunicatorClassID(rhs);
  if (lhs_class != 0 && rhs_class != 0) {
    return lhs_class == rhs_class;
  }

  return classifyCommunicatorAlias(lhs, rhs, &module_) ==
         CommunicatorAliasResult::MustAlias;
}

size_t
MPIProcessModel::getCommunicatorSubgroupID(const Value *communicator) const {
  if (!communicator) {
    return 0;
  }
  const CommunicatorTraceResult trace =
      traceCommunicatorValue(communicator, &module_);
  if (trace.state == CommunicatorTraceState::Ambiguous) {
    return 0;
  }
  const Value *key = trace.root;
  if (!key) {
    key = communicator->stripPointerCasts();
  }
  auto it = communicator_subgroup_ids_.find(key);
  return it != communicator_subgroup_ids_.end() ? it->second : 0;
}

MPICommunicatorSubgroupTokenKind
MPIProcessModel::getCommunicatorSubgroupTokenKind(
    const Value *communicator) const {
  if (!communicator) {
    return MPICommunicatorSubgroupTokenKind::None;
  }
  const CommunicatorTraceResult trace =
      traceCommunicatorValue(communicator, &module_);
  if (trace.state == CommunicatorTraceState::Ambiguous) {
    return MPICommunicatorSubgroupTokenKind::None;
  }
  const Value *key = trace.root;
  if (!key) {
    key = communicator->stripPointerCasts();
  }
  auto it = communicator_subgroup_token_kinds_.find(key);
  return it != communicator_subgroup_token_kinds_.end()
             ? it->second
             : MPICommunicatorSubgroupTokenKind::None;
}

void MPIProcessModel::buildCommunicatorFacts() {
  communicator_facts_.clear();
  std::set<size_t> seen_classes;

  for (const auto &entry : communicator_class_ids_) {
    const Value *canonical = entry.first;
    size_t class_id = entry.second;
    if (!canonical || !seen_classes.insert(class_id).second) {
      continue;
    }

    MPICommunicatorFact fact;
    fact.canonical = canonical;
    fact.communicator_class_id = class_id;
    fact.subgroup_id = getCommunicatorSubgroupID(canonical);
    auto parent_it = communicator_parents_.find(canonical);
    if (parent_it != communicator_parents_.end()) {
      fact.parent = parent_it->second;
    }
    auto creation_it = communicator_creation_kinds_.find(canonical);
    if (creation_it != communicator_creation_kinds_.end()) {
      fact.creation_kind = creation_it->second;
    } else if (isKnownWorldCommunicatorHandle(canonical)) {
      fact.creation_kind = MPICommunicatorCreationKind::World;
    }
    auto subgroup_it = communicator_process_sets_.find(canonical);
    if (subgroup_it != communicator_process_sets_.end()) {
      fact.subgroup = MPIParticipantSet::fromProcessSetFact(subgroup_it->second);
      fact.subgroup_id = subgroup_it->second.subgroup_id;
      fact.subgroup_token_kind = subgroup_it->second.subgroup_token_kind;
      fact.participant_scope = subgroup_it->second.scope_kind;
      fact.communicator_side = subgroup_it->second.communicator_side;
    }
    auto topology_it = communicator_topologies_.find(canonical);
    if (topology_it != communicator_topologies_.end()) {
      fact.topology_kind = topology_it->second;
    }
    fact.is_intercommunicator = intercommunicators_.count(canonical) != 0;

    if (rank_analysis_) {
      int min_size = 0;
      int max_size = 0;
      if (rank_analysis_->getCommunicatorSizeRange(canonical, min_size, max_size)) {
        fact.has_known_size = true;
        fact.size_min = min_size;
        fact.size_max = max_size;
      }
    }

    if (!fact.parent &&
        fact.creation_kind == MPICommunicatorCreationKind::World) {
      fact.detail = "world";
    } else if (fact.parent) {
      fact.detail = "derived";
    } else {
      fact.detail = "standalone";
    }
    communicator_facts_.push_back(fact);
  }
}

void MPIProcessModel::buildFunctionSummaries() {
  function_summaries_.clear();

  std::unordered_map<const Instruction *, std::vector<size_t>>
      operations_by_instruction;
  for (size_t idx = 0; idx < all_operations_.size(); ++idx) {
    if (all_operations_[idx].inst) {
      operations_by_instruction[all_operations_[idx].inst].push_back(idx);
    }
  }

  std::unordered_map<const Function *, size_t> summary_index_by_function;
  for (Function &function : module_) {
    if (function.isDeclaration()) {
      continue;
    }
    MPIFunctionSummary summary;
    summary.function = &function;
    for (const Instruction &inst : instructions(function)) {
      auto op_it = operations_by_instruction.find(&inst);
      if (op_it != operations_by_instruction.end()) {
        for (size_t op_index : op_it->second) {
          summary.direct_operation_indices.push_back(op_index);
          const MPIOperation &op = all_operations_[op_index];
          if (op.kind == MPIOpKind::RMA_WINDOW || op.kind == MPIOpKind::RMA_DATA ||
              op.kind == MPIOpKind::RMA_SYNC) {
            summary.rma_operation_indices.push_back(op_index);
          }
          if (op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST ||
              op.request) {
            summary.request_operation_indices.push_back(op_index);
          }
          if (isSendOperationKind(op.kind) || isRecvOperationKind(op.kind)) {
            summary.channel_operation_indices.push_back(op_index);
          }
          if (isCollectiveKind(op.kind)) {
            summary.collective_operation_indices.push_back(op_index);
          }
          if (op.communicator_class_id != 0 &&
              std::find(summary.communicator_class_ids.begin(),
                        summary.communicator_class_ids.end(),
                        op.communicator_class_id) ==
                  summary.communicator_class_ids.end()) {
            summary.communicator_class_ids.push_back(op.communicator_class_id);
          }
        }
      }
      const Function *callee = getInstructionCallee(&inst);
      if (callee && !callee->isDeclaration() &&
          std::find(summary.callees.begin(), summary.callees.end(), callee) ==
              summary.callees.end()) {
        summary.callees.push_back(callee);
      }
    }
    function_summaries_.push_back(summary);
    summary_index_by_function.emplace(&function, function_summaries_.size() - 1);
  }

  std::function<std::vector<size_t>(const Function *, std::set<const Function *> &)>
      expandFunction = [&](const Function *function,
                           std::set<const Function *> &active_stack) {
        auto summary_it = summary_index_by_function.find(function);
        if (summary_it == summary_index_by_function.end()) {
          return std::vector<size_t>{};
        }

        MPIFunctionSummary &summary = function_summaries_[summary_it->second];
        if (summary.reaches_fixed_point) {
          return summary.expanded_operation_indices;
        }
        if (!active_stack.insert(function).second) {
          summary.recursive = true;
          return summary.direct_operation_indices;
        }

        std::vector<size_t> expanded;
        for (const Instruction &inst : instructions(*function)) {
          auto op_it = operations_by_instruction.find(&inst);
          if (op_it != operations_by_instruction.end()) {
            expanded.insert(expanded.end(), op_it->second.begin(), op_it->second.end());
          }

          const Function *callee = getInstructionCallee(&inst);
          if (!callee || callee->isDeclaration()) {
            if (isa<CallBase>(&inst) && !callee) {
              MPIModelGap gap;
              gap.domain = MPIModelGapDomain::Unknown;
              gap.inst = &inst;
              gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
              gap.relation.proof = concurrency::ProofStrength::Unknown;
              gap.relation.reason = "mpi_summary_indirect_call";
              gap.code = "mpi_summary_indirect_call";
              gap.detail = function->getName().str();
              model_gaps_.push_back(gap);
            }
            continue;
          }

          auto callee_it = summary_index_by_function.find(callee);
          if (callee_it == summary_index_by_function.end()) {
            continue;
          }

          if (active_stack.count(callee) != 0) {
            summary.recursive = true;
            function_summaries_[callee_it->second].recursive = true;
            continue;
          }

          std::vector<size_t> callee_expanded = expandFunction(callee, active_stack);
          expanded.insert(expanded.end(), callee_expanded.begin(), callee_expanded.end());
        }

        active_stack.erase(function);
        summary.expanded_operation_indices = expanded;
        summary.reaches_fixed_point = true;
        summary.iterations = 1;
        return summary.expanded_operation_indices;
      };

  std::vector<const Function *> roots = collectRootFunctions(module_);
  for (const Function *root : roots) {
    std::set<const Function *> active_stack;
    expandFunction(root, active_stack);
  }
  for (MPIFunctionSummary &summary : function_summaries_) {
    if (!summary.reaches_fixed_point) {
      std::set<const Function *> active_stack;
      expandFunction(summary.function, active_stack);
    }
  }
}

void MPIProcessModel::augmentFunctionSummaries() {
  std::unordered_map<size_t, std::vector<size_t>> send_endpoints_by_op;
  std::unordered_map<size_t, std::vector<size_t>> recv_endpoints_by_op;
  for (const MPIChannelEndpointObligation &endpoint : channel_endpoint_obligations_) {
    auto &target = endpoint.endpoint_kind == MPIChannelEndpointKind::Send
                       ? send_endpoints_by_op[endpoint.operation_index]
                       : recv_endpoints_by_op[endpoint.operation_index];
    target.push_back(endpoint.obligation_id);
  }

  std::unordered_map<const Instruction *, std::vector<size_t>> created_request_sets_by_inst;
  std::unordered_map<const Instruction *, std::vector<size_t>> discharged_request_sets_by_inst;
  for (const MPIRequestSetFact &request_set : request_set_facts_) {
    if (request_set.origin_inst) {
      created_request_sets_by_inst[request_set.origin_inst].push_back(
          request_set.request_set_id);
    }
    if (!request_set.transition_inst) {
      continue;
    }
    if (request_set.state == MPIRequestState::MustComplete ||
        request_set.state == MPIRequestState::Freed ||
        request_set.state == MPIRequestState::Canceled) {
      discharged_request_sets_by_inst[request_set.transition_inst].push_back(
          request_set.request_set_id);
    }
  }

  std::unordered_map<const Instruction *, std::set<size_t>> channel_ids_by_inst;
  std::unordered_map<size_t, std::set<size_t>> request_set_ids_by_channel_class;
  for (const MPIChannelObligation &channel : channel_obligations_) {
    if (channel.channel_class_id != 0) {
      channel_ids_by_inst[channel.sender_inst].insert(channel.channel_class_id);
      channel_ids_by_inst[channel.receiver_inst].insert(channel.channel_class_id);
    }
    if (channel.channel_class_id != 0 && channel.request_set_id != 0) {
      request_set_ids_by_channel_class[channel.channel_class_id].insert(
          channel.request_set_id);
    }
  }

  std::unordered_map<const Function *, bool> has_indirect_call;
  for (Function &function : module_) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const Instruction &inst : instructions(function)) {
      if (isa<CallBase>(&inst) && !getInstructionCallee(&inst)) {
        has_indirect_call[&function] = true;
      }
    }
  }

  for (MPIFunctionSummary &summary : function_summaries_) {
    std::set<size_t> seen_send_endpoints;
    std::set<size_t> seen_recv_endpoints;
    std::set<size_t> seen_created_request_sets;
    std::set<size_t> seen_discharged_request_sets;
    std::set<size_t> seen_channel_ids;
    std::set<size_t> seen_blocking_endpoint_ids;
    std::set<size_t> seen_blocking_request_set_ids;

    summary.emitted_send_endpoint_ids.clear();
    summary.emitted_receive_endpoint_ids.clear();
    summary.created_request_set_ids.clear();
    summary.discharged_request_set_ids.clear();
    summary.touched_channel_class_ids.clear();
    summary.expanded_collective_operation_indices.clear();
    summary.outstanding_send_endpoint_ids.clear();
    summary.outstanding_receive_endpoint_ids.clear();
    summary.outstanding_request_set_ids.clear();
    summary.unresolved_channel_class_ids.clear();
    summary.collective_call_operation_indices.clear();
    summary.entered_collective_protocol_slots.clear();
    summary.outstanding_collective_frontier_ids.clear();
    summary.blocking_endpoint_obligation_ids.clear();
    summary.blocking_request_set_ids.clear();
    summary.unresolved_indirect_call_effect = has_indirect_call[summary.function];
    summary.unresolved_collective_summary_effect = false;

    for (size_t op_index : summary.expanded_operation_indices) {
      if (op_index >= all_operations_.size()) {
        continue;
      }
      const MPIOperation &op = all_operations_[op_index];
      if (isCollectiveKind(op.kind)) {
        summary.expanded_collective_operation_indices.push_back(op_index);
      }
      auto send_it = send_endpoints_by_op.find(op_index);
      if (send_it != send_endpoints_by_op.end()) {
        for (size_t obligation_id : send_it->second) {
          if (seen_send_endpoints.insert(obligation_id).second) {
            summary.emitted_send_endpoint_ids.push_back(obligation_id);
          }
          auto endpoint_it = std::find_if(
              channel_endpoint_obligations_.begin(), channel_endpoint_obligations_.end(),
              [&](const MPIChannelEndpointObligation &endpoint) {
                return endpoint.obligation_id == obligation_id;
              });
          if (endpoint_it != channel_endpoint_obligations_.end() && endpoint_it->blocking &&
              seen_blocking_endpoint_ids.insert(obligation_id).second) {
            summary.blocking_endpoint_obligation_ids.push_back(obligation_id);
          }
        }
      }
      auto recv_it = recv_endpoints_by_op.find(op_index);
      if (recv_it != recv_endpoints_by_op.end()) {
        for (size_t obligation_id : recv_it->second) {
          if (seen_recv_endpoints.insert(obligation_id).second) {
            summary.emitted_receive_endpoint_ids.push_back(obligation_id);
          }
          auto endpoint_it = std::find_if(
              channel_endpoint_obligations_.begin(), channel_endpoint_obligations_.end(),
              [&](const MPIChannelEndpointObligation &endpoint) {
                return endpoint.obligation_id == obligation_id;
              });
          if (endpoint_it != channel_endpoint_obligations_.end() && endpoint_it->blocking &&
              seen_blocking_endpoint_ids.insert(obligation_id).second) {
            summary.blocking_endpoint_obligation_ids.push_back(obligation_id);
          }
        }
      }

      if (op.inst) {
        auto created_it = created_request_sets_by_inst.find(op.inst);
        if (created_it != created_request_sets_by_inst.end()) {
          for (size_t request_set_id : created_it->second) {
            if (seen_created_request_sets.insert(request_set_id).second) {
              summary.created_request_set_ids.push_back(request_set_id);
            }
          }
        }
        auto discharged_it = discharged_request_sets_by_inst.find(op.inst);
        if (discharged_it != discharged_request_sets_by_inst.end()) {
          for (size_t request_set_id : discharged_it->second) {
            if (seen_discharged_request_sets.insert(request_set_id).second) {
              summary.discharged_request_set_ids.push_back(request_set_id);
            }
          }
        }
        auto channel_it = channel_ids_by_inst.find(op.inst);
        if (channel_it != channel_ids_by_inst.end()) {
          for (size_t channel_id : channel_it->second) {
            if (seen_channel_ids.insert(channel_id).second) {
              summary.touched_channel_class_ids.push_back(channel_id);
            }
          }
        }
      }

      if ((op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST) && op.inst) {
        auto discharged_it = discharged_request_sets_by_inst.find(op.inst);
        if (discharged_it != discharged_request_sets_by_inst.end()) {
          for (size_t request_set_id : discharged_it->second) {
            if (seen_blocking_request_set_ids.insert(request_set_id).second) {
              summary.blocking_request_set_ids.push_back(request_set_id);
            }
          }
        }
      }
    }

    for (size_t channel_id : summary.touched_channel_class_ids) {
      auto request_set_it = request_set_ids_by_channel_class.find(channel_id);
      if (request_set_it == request_set_ids_by_channel_class.end()) {
        continue;
      }
      for (size_t request_set_id : request_set_it->second) {
        if (seen_blocking_request_set_ids.insert(request_set_id).second) {
          summary.blocking_request_set_ids.push_back(request_set_id);
        }
      }
    }
  }
}

void MPIProcessModel::annotateRankConstraints(MPIOperation &op) const {
  if (!rank_analysis_ || !op.inst) {
    return;
  }

  MPI::RankExpr rank = rank_analysis_->getRankAtInstruction(op.inst);
  op.process_rank = rank;
  op.rank_predicate = rank_analysis_->getRankPredicateAtInstruction(op.inst);
  op.predicate_class_id = rank_analysis_->getPredicateClassAtInstruction(op.inst);
  op.participant_class_id =
      rank_analysis_->getParticipantClassAtInstruction(op.inst);
  op.process_set_fact = MPIProcessSetFact::fromPredicate(
      op.rank_predicate, 0, op.communicator_subgroup_id, op.predicate_class_id,
      op.participant_class_id, op.process_set_fact.subgroup_token_kind,
      op.process_set_fact.communicator_side);
  if (op.communicator) {
    op.process_set_fact.communicator = op.communicator;
  }
  op.participant_set = MPIParticipantSet::fromProcessSetFact(op.process_set_fact);
  if (!op.process_set_fact.communicator ||
      op.process_set_fact.scope_kind == MPIProcessSetScopeKind::Unknown) {
    op.protocol_reachability = ProtocolReachability::Unknown;
  } else if (op.process_set_fact.scope_kind == MPIProcessSetScopeKind::All) {
    op.protocol_reachability = ProtocolReachability::AllRanks;
  } else {
    op.protocol_reachability = ProtocolReachability::SomeRanks;
  }
  op.rank_path_summary = op.participant_set.toKey();
  auto assignRange = [](const MPI::RankExpr &expr, int concrete_value,
                        int &min_out, int &max_out) {
    if (concrete_value >= 0) {
      min_out = concrete_value;
      max_out = concrete_value;
      return;
    }
    if (expr.kind == MPI::RankExpr::Concrete) {
      min_out = expr.concrete_value;
      max_out = expr.concrete_value;
    } else if (expr.kind == MPI::RankExpr::Range) {
      min_out = expr.range_min;
      max_out = expr.range_max;
    }
  };
  assignRange(rank, op.source_rank, op.source_rank_min, op.source_rank_max);
  assignRange(rank, op.dest_rank, op.dest_rank_min, op.dest_rank_max);
  assignRange(rank, op.target_rank, op.target_rank_min, op.target_rank_max);
}

int64_t MPIProcessModel::getDatatypeExtent(const Value *datatype_arg,
                                           const Instruction *context) const {
  auto resolveBuiltinExtent = [](int datatype) {
    switch (datatype) {
    case 0:
      return int64_t(1);
    case 1:
      return int64_t(2);
    case 2:
      return int64_t(4);
    case 3:
      return int64_t(8);
    default:
      return int64_t(-1);
    }
  };

  const Value *canonical = canonicalizeDatatypeHandle(datatype_arg);
  if (canonical) {
    auto it = datatype_extent_bytes_.find(canonical);
    if (it != datatype_extent_bytes_.end()) {
      return it->second;
    }
  }

  if (const auto *load = dyn_cast_or_null<LoadInst>(datatype_arg)) {
    const Value *loaded_from =
        canonicalizeDatatypeHandle(load->getPointerOperand());
    if (loaded_from) {
      auto it = datatype_extent_bytes_.find(loaded_from);
      if (it != datatype_extent_bytes_.end()) {
        return it->second;
      }
    }
  }

  int datatype = 0;
  if (tryReadScalarInt(datatype_arg, datatype, context)) {
    return resolveBuiltinExtent(datatype);
  }
  return -1;
}

const Value *
MPIProcessModel::canonicalizeDatatypeHandle(const Value *handle) const {
  if (!handle) {
    return nullptr;
  }
  handle = handle->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(handle)) {
    handle = underlying->stripPointerCasts();
  }
  return handle;
}

void MPIProcessModel::registerDatatypeExtent(const Value *handle,
                                             int64_t extent) {
  if (!handle || extent <= 0) {
    return;
  }
  datatype_extent_bytes_[canonicalizeDatatypeHandle(handle)] = extent;
}

void MPIProcessModel::extractPointToPointDetails(
    MPIOperation &op, const CallBase *cb,
    const MPISemanticDescriptor &descriptor) {
  if (!cb) {
    return;
  }

  op.matched_message = op.td_type == ThreadAPI::TD_MPI_IMRECV ||
                       op.td_type == ThreadAPI::TD_MPI_MRECV;

  const Value *datatype = getOperandBySignedIndex(cb, descriptor.datatype_arg);
  op.datatype = datatype;
  op.datatype_size = getDatatypeExtent(op.datatype, op.inst);

  const Value *count_arg = getOperandBySignedIndex(cb, descriptor.count_arg);
  int count = 0;
  if (count_arg && tryReadScalarInt(count_arg, count, op.inst) &&
      op.datatype_size > 0) {
    op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
  }

  const Value *peer_arg = getOperandBySignedIndex(cb, descriptor.peer_rank_arg);
  if (peer_arg) {
    int value = -1;
    if (tryGetConstantInt(peer_arg, value)) {
      if (descriptor.peer_rank_is_dest) {
        op.dest_rank = value;
      } else {
        op.source_rank = value;
      }
    } else if (descriptor.peer_rank_is_dest) {
      tryGetScalarRange(peer_arg, op.dest_rank_min, op.dest_rank_max);
    } else {
      tryGetScalarRange(peer_arg, op.source_rank_min, op.source_rank_max);
    }
  }

  const Value *tag_arg = getOperandBySignedIndex(cb, descriptor.tag_arg);
  if (tag_arg) {
    int value = -1;
    if (tryGetConstantInt(tag_arg, value)) {
      op.tag = value;
    }
  }

  const Value *comm_arg =
      getOperandBySignedIndex(cb, descriptor.communicator_arg);
  if (comm_arg) {
    op.communicator = canonicalizeCommunicator(comm_arg);
    op.communicator_subgroup_id = getCommunicatorSubgroupID(comm_arg);
    op.process_set_fact.subgroup_token_kind =
        getCommunicatorSubgroupTokenKind(comm_arg);
  }

  const Value *request_arg =
      getOperandBySignedIndex(cb, descriptor.request_arg);
  if (request_arg) {
    op.request = request_arg;
  }
}

void MPIProcessModel::extractSendrecvDetails(MPIOperation &op,
                                             const CallBase *cb) const {
  if (!cb) {
    return;
  }

  unsigned num_args = cb->arg_size();
  int value = -1;
  if (num_args >= 11) {
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      op.datatype = cb->getArgOperand(2);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(1), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(3), op.dest_rank_min,
                          op.dest_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
    } else {
      op.datatype = cb->getArgOperand(7);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(6), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(8), value)) {
        op.source_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(8), op.source_rank_min,
                          op.source_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(9), value)) {
        op.tag = value;
      }
    }
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(10));
    op.communicator_subgroup_id = getCommunicatorSubgroupID(cb->getArgOperand(10));
    op.process_set_fact.subgroup_token_kind =
        getCommunicatorSubgroupTokenKind(cb->getArgOperand(10));
    return;
  }

  if (num_args >= 8) {
    if (op.kind == MPIOpKind::SEND_BLOCKING) {
      op.datatype = cb->getArgOperand(2);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(1), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(3), value)) {
        op.dest_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(3), op.dest_rank_min,
                          op.dest_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(4), value)) {
        op.tag = value;
      }
    } else {
      op.datatype = cb->getArgOperand(4);
      op.datatype_size = getDatatypeExtent(op.datatype, op.inst);
      int count = 0;
      if (tryReadScalarInt(cb->getArgOperand(3), count, op.inst) &&
          op.datatype_size > 0) {
        op.byte_length = static_cast<int64_t>(count) * op.datatype_size;
      }
      if (tryGetConstantInt(cb->getArgOperand(5), value)) {
        op.source_rank = value;
      } else {
        tryGetScalarRange(cb->getArgOperand(5), op.source_rank_min,
                          op.source_rank_max);
      }
      if (tryGetConstantInt(cb->getArgOperand(6), value)) {
        op.tag = value;
      }
    }
    op.communicator = canonicalizeCommunicator(cb->getArgOperand(7));
    op.communicator_subgroup_id = getCommunicatorSubgroupID(cb->getArgOperand(7));
    op.process_set_fact.subgroup_token_kind =
        getCommunicatorSubgroupTokenKind(cb->getArgOperand(7));
  }
}

void MPIProcessModel::extractProbeDetails(
    MPIOperation &op, const CallBase *cb,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }
  const Value *peer_arg = getOperandBySignedIndex(cb, descriptor.peer_rank_arg);
  if (peer_arg) {
    int value = -1;
    if (tryGetConstantInt(peer_arg, value)) {
      op.source_rank = value;
    } else {
      tryGetScalarRange(peer_arg, op.source_rank_min, op.source_rank_max);
    }
  }

  const Value *tag_arg = getOperandBySignedIndex(cb, descriptor.tag_arg);
  if (tag_arg) {
    int value = -1;
    if (tryGetConstantInt(tag_arg, value)) {
      op.tag = value;
    }
  }

  const Value *comm_arg =
      getOperandBySignedIndex(cb, descriptor.communicator_arg);
  if (comm_arg) {
    op.communicator = canonicalizeCommunicator(comm_arg);
    op.communicator_subgroup_id = getCommunicatorSubgroupID(comm_arg);
    op.process_set_fact.subgroup_token_kind =
        getCommunicatorSubgroupTokenKind(comm_arg);
  }
}

void MPIProcessModel::extractCollectiveDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  const bool nonblocking = op.kind == MPIOpKind::BARRIER_NONBLOCKING ||
                           op.kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
  int comm_index = descriptor.communicator_arg;
  if (nonblocking && descriptor.collective_nonblocking_comm_arg != -1) {
    comm_index = descriptor.collective_nonblocking_comm_arg;
  }
  const Value *comm_arg = getOperandBySignedIndex(cb, comm_index);
  if (comm_arg) {
    op.communicator = canonicalizeCommunicator(comm_arg);
    op.communicator_subgroup_id = getCommunicatorSubgroupID(comm_arg);
    op.process_set_fact.subgroup_token_kind =
        getCommunicatorSubgroupTokenKind(comm_arg);
  }

  if (nonblocking) {
    const Value *request_arg = getOperandBySignedIndex(
        cb, descriptor.collective_nonblocking_request_arg);
    if (request_arg) {
      op.request = request_arg;
    }
  }

  if (semantic_tag.startswith("neighbor-") ||
      semantic_tag.startswith("ineighbor-")) {
    op.collective_protocol_class_id = 1;
  } else if (semantic_tag.startswith("intercomm-")) {
    op.collective_protocol_class_id = 2;
  } else {
    op.collective_protocol_class_id = 0;
  }
}

void MPIProcessModel::extractRequestDetails(
    MPIOperation &op, const CallBase *cb,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }
  const Value *request_arg =
      getOperandBySignedIndex(cb, descriptor.request_arg);
  if (request_arg) {
    op.request = request_arg;
    return;
  }

  if (op.td_type == ThreadAPI::TD_MPI_REQUEST_START) {
    op.request = getOperandBySignedIndex(cb, 0);
  }
}

void MPIProcessModel::extractRMAWindowDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }
  if (StringRef(semantic_tag).startswith("win-create") ||
      StringRef(semantic_tag).equals("win-allocate") ||
      StringRef(semantic_tag).equals("win-allocate-shared")) {
    if (cb->arg_size() >= 2) {
      int comm_index = descriptor.communicator_arg;
      if ((semantic_tag.equals("win-allocate") ||
           semantic_tag.equals("win-allocate-shared")) &&
          cb->arg_size() >= 6) {
        comm_index = -3;
      }
      const Value *comm_arg = getOperandBySignedIndex(cb, comm_index);
      const Value *window_arg =
          getOperandBySignedIndex(cb, descriptor.result_handle_arg);
      op.communicator = canonicalizeCommunicator(comm_arg);
      op.window = window_arg;
    }
    return;
  }

  if (StringRef(semantic_tag).equals("win-free")) {
    op.window = getOperandBySignedIndex(cb, descriptor.window_arg);
  }
}

void MPIProcessModel::extractRMADataDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }

  auto setByteLength = [&](int count_idx, int datatype_idx) {
    const Value *count_value = getOperandBySignedIndex(cb, count_idx);
    const Value *datatype_value = getOperandBySignedIndex(cb, datatype_idx);
    if (!count_value || !datatype_value) {
      return;
    }
    int count = 0;
    if (!tryReadScalarInt(count_value, count, op.inst)) {
      return;
    }
    int64_t extent = getDatatypeExtent(datatype_value, op.inst);
    if (extent <= 0) {
      return;
    }
    op.byte_length = static_cast<int64_t>(count) * extent;
  };

  int count_idx = descriptor.count_arg;
  int datatype_idx = descriptor.datatype_arg;
  int rank_idx = descriptor.target_rank_arg;
  int disp_idx = descriptor.target_disp_arg;
  int window_idx = descriptor.window_arg;

  if (StringRef(semantic_tag).equals("get-accumulate") ||
      StringRef(semantic_tag).equals("rget-accumulate")) {
    count_idx = 4;
    datatype_idx = 5;
    rank_idx = 6;
    disp_idx = 7;
    window_idx = 11;
  } else if (StringRef(semantic_tag).equals("fetch-and-op")) {
    count_idx = -1;
    datatype_idx = 2;
    rank_idx = 3;
    disp_idx = 4;
    window_idx = 6;
  } else if (StringRef(semantic_tag).equals("compare-and-swap")) {
    count_idx = -1;
    datatype_idx = 3;
    rank_idx = 4;
    disp_idx = 5;
    window_idx = 6;
  }

  if (count_idx >= 0 && datatype_idx >= 0) {
    setByteLength(count_idx, datatype_idx);
  } else if (datatype_idx >= 0) {
    const Value *datatype_value = getOperandBySignedIndex(cb, datatype_idx);
    int64_t extent = getDatatypeExtent(datatype_value, op.inst);
    if (extent > 0) {
      op.byte_length = extent;
    } else {
      op.byte_length = 1;
    }
  }

  const Value *target_rank_arg = getOperandBySignedIndex(cb, rank_idx);
  if (target_rank_arg) {
    int value = -1;
    if (tryGetConstantInt(target_rank_arg, value)) {
      op.target_rank = value;
    } else {
      tryGetScalarRange(target_rank_arg, op.target_rank_min,
                        op.target_rank_max);
    }
  }

  const Value *disp_arg = getOperandBySignedIndex(cb, disp_idx);
  if (const auto *disp = dyn_cast_or_null<ConstantInt>(disp_arg)) {
    op.target_disp = disp->getSExtValue();
  }

  op.window = getOperandBySignedIndex(cb, window_idx);
}

void MPIProcessModel::extractRMASyncDetails(
    MPIOperation &op, const CallBase *cb, StringRef semantic_tag,
    const MPISemanticDescriptor &descriptor) const {
  if (!cb) {
    return;
  }

  StringRef tag = semantic_tag;
  if (tag.equals("win-fence")) {
    op.window = getOperandBySignedIndex(cb, 1);
    return;
  }

  if (tag.equals("win-lock")) {
    op.rma_lock_all = false;
    const Value *target_rank_arg = getOperandBySignedIndex(cb, 1);
    if (target_rank_arg) {
      int value = -1;
      if (tryGetConstantInt(target_rank_arg, value)) {
        op.target_rank = value;
      } else {
        tryGetScalarRange(target_rank_arg, op.target_rank_min,
                          op.target_rank_max);
      }
    }
    op.window = getOperandBySignedIndex(cb, 3);
    if (op.target_rank < 0 && op.target_rank_min < 0 && op.target_rank_max < 0) {
      op.semantic_relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
      op.semantic_relation.proof = concurrency::ProofStrength::Unknown;
      op.semantic_relation.reason = "mpi_rma_lock_target_unresolved";
    }
    return;
  }

  if (tag.equals("win-lock-all")) {
    op.rma_lock_all = true;
    op.window = getOperandBySignedIndex(cb, 1);
    return;
  }

  if (tag.equals("win-unlock") || tag.equals("win-flush") ||
      tag.equals("win-flush-local")) {
    op.rma_lock_all = false;
    const Value *target_rank_arg = getOperandBySignedIndex(cb, 0);
    if (target_rank_arg) {
      int value = -1;
      if (tryGetConstantInt(target_rank_arg, value)) {
        op.target_rank = value;
      } else {
        tryGetScalarRange(target_rank_arg, op.target_rank_min,
                          op.target_rank_max);
      }
    }
    op.window = getOperandBySignedIndex(cb, 1);
    op.rma_local_completion_only = tag.equals("win-flush-local");
    return;
  }

  if (tag.equals("win-unlock-all") || tag.equals("win-flush-all") ||
      tag.equals("win-flush-local-all")) {
    op.rma_lock_all = true;
    op.window = getOperandBySignedIndex(cb, 0);
    op.rma_local_completion_only = tag.equals("win-flush-local-all");
    return;
  }

  if (tag.equals("win-sync") || tag.equals("win-complete") ||
      tag.equals("win-wait") || tag.equals("win-test")) {
    op.window = getOperandBySignedIndex(cb, 0);
    return;
  }

  if (tag.equals("win-post") || tag.equals("win-start")) {
    op.group = getOperandBySignedIndex(cb, 0);
    op.window = getOperandBySignedIndex(cb, 2);
    return;
  }

  if (descriptor.group_arg != -1) {
    op.group = getOperandBySignedIndex(cb, descriptor.group_arg);
  }
  if (descriptor.window_arg != -1) {
    op.window = getOperandBySignedIndex(cb, descriptor.window_arg);
  }
}

void MPIProcessModel::extractDatatypeDetails(MPIOperation &op,
                                             const CallBase *cb,
                                             StringRef semantic_tag) {
  if (!cb) {
    return;
  }

  auto readConstExtent = [&](const Value *value) -> int64_t {
    int int_value = 0;
    if (!tryReadScalarInt(value, int_value, op.inst)) {
      return -1;
    }
    return static_cast<int64_t>(int_value);
  };

  if (semantic_tag.equals("type-contiguous") && cb->arg_size() >= 3) {
    int64_t count = readConstExtent(cb->getArgOperand(0));
    int64_t base_extent = getDatatypeExtent(cb->getArgOperand(1), op.inst);
    if (count > 0 && base_extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(2), count * base_extent);
      op.is_derived_datatype = true;
      op.datatype_size = count * base_extent;
      op.datatype = cb->getArgOperand(2);
    }
    return;
  }

  if ((semantic_tag.equals("type-vector") ||
       semantic_tag.equals("type-hvector") ||
       semantic_tag.equals("type-create-hvector")) &&
      cb->arg_size() >= 5) {
    int64_t count = readConstExtent(cb->getArgOperand(0));
    int64_t blocklength = readConstExtent(cb->getArgOperand(1));
    int64_t base_extent = getDatatypeExtent(cb->getArgOperand(3), op.inst);
    if (count > 0 && blocklength > 0 && base_extent > 0) {
      int64_t extent = count * blocklength * base_extent;
      registerDatatypeExtent(cb->getArgOperand(4), extent);
      op.is_derived_datatype = true;
      op.datatype_size = extent;
      op.datatype = cb->getArgOperand(4);
    }
    return;
  }

  if (semantic_tag.equals("type-create-resized") && cb->arg_size() >= 4) {
    int64_t extent = readConstExtent(cb->getArgOperand(2));
    if (extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(3), extent);
      op.is_derived_datatype = true;
      op.datatype_size = extent;
      op.datatype = cb->getArgOperand(3);
    }
    return;
  }

  if (semantic_tag.equals("type-create-subarray") && cb->arg_size() >= 8) {
    int64_t base_extent = getDatatypeExtent(cb->getArgOperand(6), op.inst);
    if (base_extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(7), base_extent);
      op.is_derived_datatype = true;
      op.datatype_size = base_extent;
      op.datatype = cb->getArgOperand(7);
    }
    return;
  }

  if (semantic_tag.equals("type-commit") && cb->arg_size() >= 1) {
    int64_t existing_extent = getDatatypeExtent(cb->getArgOperand(0), op.inst);
    if (existing_extent > 0) {
      registerDatatypeExtent(cb->getArgOperand(0), existing_extent);
    }
  }
}

void MPIProcessModel::extractOperationDetails(MPIOperation &op,
                                             const MPIEffect &effect) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb) {
    return;
  }
  if (!effect.has_descriptor) {
    return;
  }
  const StringRef semantic_tag = effect.semantic_tag;
  const MPISemanticDescriptor &descriptor = effect.descriptor;
  op.request_lifecycle_issue_nonblocking =
      descriptor.request_lifecycle_issue_nonblocking;
  if (descriptor.request_arg != -1) {
    op.request = getOperandBySignedIndex(cb, descriptor.request_arg);
  }

  switch (descriptor.family) {
  case MPISemanticFamily::PointToPoint:
    if (descriptor.split_into_sendrecv) {
      extractSendrecvDetails(op, cb);
    } else {
      extractPointToPointDetails(op, cb, descriptor);
    }
    break;
  case MPISemanticFamily::Probe:
    extractProbeDetails(op, cb, descriptor);
    break;
  case MPISemanticFamily::Request:
    if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT ||
        op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT) {
      extractPointToPointDetails(op, cb, descriptor);
    }
    extractRequestDetails(op, cb, descriptor);
    break;
  case MPISemanticFamily::Collective:
    extractCollectiveDetails(op, cb, semantic_tag, descriptor);
    break;
  case MPISemanticFamily::RMAWindow:
    extractRMAWindowDetails(op, cb, semantic_tag, descriptor);
    break;
  case MPISemanticFamily::RMAData:
    extractRMADataDetails(op, cb, semantic_tag, descriptor);
    break;
  case MPISemanticFamily::RMASync:
    extractRMASyncDetails(op, cb, semantic_tag, descriptor);
    break;
  case MPISemanticFamily::Datatype:
    extractDatatypeDetails(op, cb, semantic_tag);
    break;
  default:
    break;
  }
}

void MPIProcessModel::analyzeModule() {
  all_operations_.clear();
  non_blocking_ops_.clear();
  request_state_summaries_.clear();
  request_set_facts_.clear();
  semantic_events_.clear();
  point_to_point_obligations_.clear();
  channel_endpoint_obligations_.clear();
  channel_obligations_.clear();
  process_set_facts_.clear();
  participant_sets_.clear();
  model_gaps_.clear();
  communicator_facts_.clear();
  function_summaries_.clear();
  operation_kind_counts_.clear();
  canonical_communicators_.clear();
  communicator_class_ids_.clear();
  next_communicator_class_id_ = 1;
  communicator_subgroup_ids_.clear();
  communicator_subgroup_token_kinds_.clear();
  next_communicator_subgroup_id_ = 1;
  communicator_parents_.clear();
  communicator_creation_kinds_.clear();
  communicator_topologies_.clear();
  communicator_process_sets_.clear();
  communicator_size_ranges_.clear();
  intercommunicators_.clear();
  deferred_lowering_stats_.clear();
  datatype_extent_bytes_.clear();
  normalization_confidence_counts_.clear();
  init_thread_required_level_ = -1;
  has_init_thread_level_ = false;
  init_thread_provided_level_ = -1;
  has_provided_init_thread_level_ = false;
  rank_analysis_ = std::make_unique<MPI::MPIRankAnalysis>(module_);
  rank_analysis_->analyze();

  for (Function &F : module_) {
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;

      const Function *callee = thread_api_->getCallee(I);
      if (!callee)
        continue;

      MPIEffect effect = buildMPIEffect(I, thread_api_);
      ThreadAPI::TD_TYPE type = effect.type;
      if (type == ThreadAPI::TD_DUMMY || !effect.has_descriptor)
        continue;
      normalization_confidence_counts_[effect.confidence]++;
      const auto *cb = dyn_cast<CallBase>(I);
      const Value *communicator_operand = getCommunicatorOperand(cb, effect);
      const bool communicator_identity_ambiguous =
          communicator_operand &&
          traceCommunicatorValue(communicator_operand, &module_).state ==
              CommunicatorTraceState::Ambiguous;

      const MPISemanticDescriptor &descriptor = effect.descriptor;
      if (descriptor.split_into_sendrecv) {
        MPIOperation send_op(I, MPIOpKind::SEND_BLOCKING, type);
        send_op.normalization_confidence = effect.confidence;
        send_op.send_mode = effect.send_mode;
        send_op.blocking_mode = effect.blocking_mode;
        send_op.request_arity = effect.request_arity;
        send_op.collective_variant = effect.collective_variant;
        send_op.collective_shape = effect.collective_shape;
        send_op.rma_access_kind = effect.rma_access_kind;
        send_op.rma_sync_kind = effect.rma_sync_kind;
        send_op.rma_local_completion_only = effect.rma_local_completion_only;
        extractOperationDetails(send_op, effect);
        annotateRankConstraints(send_op);
        if (send_op.communicator) {
          send_op.communicator_class_id = assignCommunicatorClass(
              canonicalizeCommunicator(send_op.communicator));
          send_op.process_set_fact.communicator = send_op.communicator;
          send_op.process_set_fact.communicator_class_id =
              send_op.communicator_class_id;
          send_op.process_set_fact.subgroup_id = send_op.communicator_subgroup_id;
          send_op.participant_set.communicator = send_op.communicator;
          send_op.participant_set = MPIParticipantSet::fromProcessSetFact(
              send_op.process_set_fact);
        }
        all_operations_.push_back(send_op);
        ++operation_kind_counts_[send_op.kind];
        if (communicator_identity_ambiguous) {
          MPIModelGap gap;
          gap.domain = MPIModelGapDomain::Communicator;
          gap.inst = send_op.inst;
          gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
          gap.relation.proof = concurrency::ProofStrength::Unknown;
          gap.relation.reason = "mpi_communicator_identity_ambiguous";
          gap.code = gap.relation.reason;
          gap.detail = send_op.function ? send_op.function->getName().str() : "";
          model_gaps_.push_back(gap);
        }

        MPIOperation recv_op(I, MPIOpKind::RECV_BLOCKING, type);
        recv_op.normalization_confidence = effect.confidence;
        recv_op.send_mode = effect.send_mode;
        recv_op.blocking_mode = effect.blocking_mode;
        recv_op.request_arity = effect.request_arity;
        recv_op.collective_variant = effect.collective_variant;
        recv_op.collective_shape = effect.collective_shape;
        recv_op.rma_access_kind = effect.rma_access_kind;
        recv_op.rma_sync_kind = effect.rma_sync_kind;
        recv_op.rma_local_completion_only = effect.rma_local_completion_only;
        extractOperationDetails(recv_op, effect);
        annotateRankConstraints(recv_op);
        if (recv_op.communicator) {
          recv_op.communicator_class_id = assignCommunicatorClass(
              canonicalizeCommunicator(recv_op.communicator));
          recv_op.process_set_fact.communicator = recv_op.communicator;
          recv_op.process_set_fact.communicator_class_id =
              recv_op.communicator_class_id;
          recv_op.process_set_fact.subgroup_id = recv_op.communicator_subgroup_id;
          recv_op.participant_set.communicator = recv_op.communicator;
          recv_op.participant_set = MPIParticipantSet::fromProcessSetFact(
              recv_op.process_set_fact);
        }
        all_operations_.push_back(recv_op);
        ++operation_kind_counts_[recv_op.kind];
        if (communicator_identity_ambiguous) {
          MPIModelGap gap;
          gap.domain = MPIModelGapDomain::Communicator;
          gap.inst = recv_op.inst;
          gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
          gap.relation.proof = concurrency::ProofStrength::Unknown;
          gap.relation.reason = "mpi_communicator_identity_ambiguous";
          gap.code = gap.relation.reason;
          gap.detail = recv_op.function ? recv_op.function->getName().str() : "";
          model_gaps_.push_back(gap);
        }
        continue;
      }

      MPIOpKind kind = effect.kind;
      if (kind == MPIOpKind::UNKNOWN)
        continue;

      MPIOperation op(I, kind, type);
      op.normalization_confidence = effect.confidence;
      op.send_mode = effect.send_mode;
      op.blocking_mode = effect.blocking_mode;
      op.request_arity = effect.request_arity;
      op.collective_variant = effect.collective_variant;
      op.collective_shape = effect.collective_shape;
      op.rma_access_kind = effect.rma_access_kind;
      op.rma_sync_kind = effect.rma_sync_kind;
      op.rma_local_completion_only = effect.rma_local_completion_only;
      extractOperationDetails(op, effect);

      if (kind == MPIOpKind::INIT && callee) {
        std::string semantic_tag_storage = thread_api_->getSemanticTag(callee);
        StringRef semantic_tag = semantic_tag_storage;
        if (semantic_tag.equals("init-thread")) {
          const auto *cb = dyn_cast<CallBase>(I);
          int required_level = -1;
          if (cb && tryReadScalarInt(getOperandBySignedIndex(cb, 2),
                                     required_level, I)) {
            has_init_thread_level_ = true;
            init_thread_required_level_ = required_level;
          }
          int provided_level = -1;
          if (cb && tryReadScalarInt(getOperandBySignedIndex(cb, 3),
                                     provided_level, I)) {
            has_provided_init_thread_level_ = true;
            init_thread_provided_level_ = provided_level;
          }
        }
      }

      annotateRankConstraints(op);
      if (op.communicator) {
        op.communicator_class_id =
            assignCommunicatorClass(canonicalizeCommunicator(op.communicator));
        op.process_set_fact.communicator = op.communicator;
        op.process_set_fact.communicator_class_id = op.communicator_class_id;
        op.process_set_fact.subgroup_id = op.communicator_subgroup_id;
        op.participant_set.communicator = op.communicator;
        op.participant_set =
            MPIParticipantSet::fromProcessSetFact(op.process_set_fact);
      }

      if (!op.matched_message &&
          op.protocol_reachability == ProtocolReachability::Unknown) {
        MPIModelGap gap;
        gap.domain = !op.communicator ? MPIModelGapDomain::Communicator
                                      : MPIModelGapDomain::ParticipantSet;
        gap.inst = op.inst;
        gap.communicator = op.communicator;
        gap.communicator_class_id = op.communicator_class_id;
        gap.subgroup_id = op.communicator_subgroup_id;
        gap.participant_class_id = op.participant_class_id;
        gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
        gap.relation.proof = concurrency::ProofStrength::Unknown;
        gap.relation.reason =
            !op.communicator
                ? (communicator_identity_ambiguous
                       ? "mpi_communicator_identity_ambiguous"
                       : "mpi_communicator_identity_unresolved")
                : "mpi_participant_scope_unresolved";
        gap.code = gap.relation.reason;
        gap.detail = op.rank_path_summary;
        model_gaps_.push_back(gap);
      }

      if (kind == MPIOpKind::COMM_MANAGEMENT ||
          kind == MPIOpKind::INTERCOMM_CREATION) {
        if (const auto *cb = dyn_cast<CallBase>(I)) {
          const Value *root = getOperandBySignedIndex(cb, descriptor.communicator_arg);
          const Value *derived_handle =
              getOperandBySignedIndex(cb, descriptor.result_handle_arg);
          MPICommunicatorCreationKind descriptor_creation_kind =
              MPICommunicatorCreationKind::Unknown;
          switch (descriptor.communicator_semantic) {
          case MPICommunicatorSemanticKind::Duplicate:
            descriptor_creation_kind = MPICommunicatorCreationKind::Dup;
            break;
          case MPICommunicatorSemanticKind::Split:
            descriptor_creation_kind = MPICommunicatorCreationKind::Split;
            break;
          case MPICommunicatorSemanticKind::Create:
            descriptor_creation_kind = MPICommunicatorCreationKind::Create;
            break;
          case MPICommunicatorSemanticKind::IntercommunicatorCreate:
            descriptor_creation_kind =
                MPICommunicatorCreationKind::IntercommCreate;
            break;
          case MPICommunicatorSemanticKind::TopologyCreate:
            descriptor_creation_kind = MPICommunicatorCreationKind::Topology;
            break;
          case MPICommunicatorSemanticKind::Free:
          case MPICommunicatorSemanticKind::None:
            break;
          }
          std::string semantic_tag_storage =
              callee ? thread_api_->getSemanticTag(callee) : std::string();
          StringRef semantic_tag = semantic_tag_storage;

          if (kind == MPIOpKind::INTERCOMM_CREATION && derived_handle) {
            recordCommunicatorCreation(derived_handle, root,
                                       descriptor_creation_kind,
                                       nullptr, "", true);
            registerCommunicatorAlias(derived_handle, root);
          } else if (type == ThreadAPI::TD_MPI_COMM_DUP && derived_handle) {
            recordCommunicatorCreation(derived_handle, root,
                                       descriptor_creation_kind);
            registerCommunicatorAlias(derived_handle, root);
            size_t subgroup_id = getCommunicatorSubgroupID(root);
            if (subgroup_id != 0) {
              const CommunicatorTraceResult alias_trace =
                  traceCommunicatorValue(derived_handle, &module_);
              const Value *alias_key = alias_trace.root;
              if (!alias_key) {
                alias_key = derived_handle->stripPointerCasts();
              }
              communicator_subgroup_ids_[alias_key] = subgroup_id;
              communicator_subgroup_token_kinds_[alias_key] =
                  getCommunicatorSubgroupTokenKind(root);
            }
          } else if (type == ThreadAPI::TD_MPI_COMM_SPLIT && derived_handle) {
            if (semantic_tag.equals("comm-split-type")) {
              registerCommunicatorSubgroup(
                  derived_handle, root,
                  MPICommunicatorSubgroupTokenKind::SplitColorUnknown);
              MPIProcessSetFact subgroup;
              subgroup.communicator = canonicalizeCommunicator(root);
              subgroup.communicator_class_id =
                  assignCommunicatorClass(subgroup.communicator);
              subgroup.unknown = true;
              subgroup.universal = true;
              subgroup.scope_kind = MPIProcessSetScopeKind::Unknown;
              subgroup.provenance = "comm-split-type";
              subgroup.subgroup_token_kind =
                  MPICommunicatorSubgroupTokenKind::SplitColorUnknown;
              subgroup.subgroup_id = getCommunicatorSubgroupID(derived_handle);
              recordCommunicatorCreation(derived_handle, root,
                                         MPICommunicatorCreationKind::Split,
                                         &subgroup);
              MPIModelGap gap;
              gap.domain = MPIModelGapDomain::Communicator;
              gap.inst = I;
              gap.communicator = canonicalizeCommunicator(root);
              gap.communicator_class_id =
                  assignCommunicatorClass(gap.communicator);
              gap.subgroup_id = subgroup.subgroup_id;
              gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
              gap.relation.proof = concurrency::ProofStrength::Unknown;
              gap.relation.reason = "mpi_subgroup_identity_unresolved";
              gap.code = "mpi_subgroup_identity_unresolved";
              gap.detail = "MPI_Comm_split_type subgroup identity is not modeled";
              model_gaps_.push_back(gap);
            } else {
            int color = 0;
            MPIProcessSetFact subgroup;
            subgroup.communicator = canonicalizeCommunicator(root);
            subgroup.communicator_class_id =
                assignCommunicatorClass(subgroup.communicator);
            subgroup.unknown = false;
            subgroup.universal = true;
              subgroup.scope_kind = MPIProcessSetScopeKind::All;
              subgroup.provenance = "comm-split";
              if (tryReadScalarInt(cb->getArgOperand(1), color, I)) {
                registerCommunicatorSubgroup(
                    derived_handle, root,
                    MPICommunicatorSubgroupTokenKind::SplitColorConst, color);
                subgroup.subgroup_token_kind =
                    MPICommunicatorSubgroupTokenKind::SplitColorConst;
                subgroup.subgroup_id = getCommunicatorSubgroupID(derived_handle);
                recordCommunicatorCreation(derived_handle, root,
                                           MPICommunicatorCreationKind::Split,
                                           &subgroup);
              } else {
                registerCommunicatorSubgroup(
                    derived_handle, root,
                    MPICommunicatorSubgroupTokenKind::SplitColorUnknown);
                subgroup.subgroup_token_kind =
                    MPICommunicatorSubgroupTokenKind::SplitColorUnknown;
                subgroup.subgroup_id = getCommunicatorSubgroupID(derived_handle);
                recordCommunicatorCreation(derived_handle, root,
                                           MPICommunicatorCreationKind::Split,
                                           &subgroup);
              MPIModelGap gap;
              gap.domain = MPIModelGapDomain::Communicator;
              gap.inst = I;
              gap.communicator = canonicalizeCommunicator(root);
              gap.communicator_class_id = assignCommunicatorClass(gap.communicator);
              gap.subgroup_id = subgroup.subgroup_id;
              gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
              gap.relation.proof = concurrency::ProofStrength::Unknown;
              gap.relation.reason = "mpi_subgroup_identity_unresolved";
              gap.code = "mpi_subgroup_identity_unresolved";
              gap.detail = "MPI_Comm_split color is not constant";
              model_gaps_.push_back(gap);
            }
            }
          } else if (type == ThreadAPI::TD_MPI_COMM_CREATE && derived_handle) {
            MPICommunicatorCreationKind creation_kind =
                semantic_tag.startswith("topology-")
                    ? MPICommunicatorCreationKind::Topology
                    : descriptor_creation_kind;
            recordCommunicatorCreation(derived_handle, root,
                                       creation_kind, nullptr,
                                       creation_kind ==
                                               MPICommunicatorCreationKind::Topology
                                           ? semantic_tag
                                           : "");
            registerCommunicatorAlias(derived_handle, root);
          } else if ((type == ThreadAPI::TD_MPI_CART_CREATE ||
                      type == ThreadAPI::TD_MPI_CART_SUB ||
                      type == ThreadAPI::TD_MPI_DIST_GRAPH_CREATE ||
                      type == ThreadAPI::TD_MPI_DIST_GRAPH_CREATE_ADJACENT ||
                      type == ThreadAPI::TD_MPI_GRAPH_CREATE) &&
                     derived_handle) {
            recordCommunicatorCreation(derived_handle, root,
                                       descriptor_creation_kind,
                                       nullptr, semantic_tag);
            registerCommunicatorAlias(derived_handle, root);
          }
        }
      }

      all_operations_.push_back(op);
      ++operation_kind_counts_[kind];
    }
  }

  {
    std::set<std::string> seen_process_sets;
    std::set<std::string> seen_participants;
    for (const MPIOperation &op : all_operations_) {
      if (!op.process_set_fact.provenance.empty()) {
        const std::string process_key =
            op.participant_set.toKey() + ":" + op.process_set_fact.provenance;
        if (seen_process_sets.insert(process_key).second) {
          process_set_facts_.push_back(op.process_set_fact);
        }
      }
      if (!seen_participants.insert(op.participant_set.toKey()).second) {
        continue;
      }
      participant_sets_.push_back(op.participant_set);
    }
  }

  buildCommunicatorFacts();
  buildFunctionSummaries();

  buildSemanticEvents();
  analyzeRequestStateDomain();
  buildPointToPointObligations();
  augmentFunctionSummaries();

  if (!deferred_lowering_stats_.empty()) {
    errs() << "MPI deferred lowering:";
    for (const auto &entry : deferred_lowering_stats_) {
      errs() << " " << entry.first << "=" << entry.second;
    }
    errs() << "\n";
  }
}

std::vector<RequestID>
MPIProcessModel::collectRequestOperands(const Value *request_arg,
                                        const Instruction *context) const {
  std::vector<RequestID> requests;
  if (!request_arg) {
    return requests;
  }

  const bool likely_request_array =
      (request_arg->getType()->isPointerTy() &&
       request_arg->getType()->getPointerElementType()->isPointerTy()) ||
      (canonicalMemoryBase(request_arg) &&
       ((isa<AllocaInst>(canonicalMemoryBase(request_arg)) &&
         cast<AllocaInst>(canonicalMemoryBase(request_arg))
             ->getAllocatedType()
             ->isArrayTy()) ||
        (isa<GlobalVariable>(canonicalMemoryBase(request_arg)) &&
         cast<GlobalVariable>(canonicalMemoryBase(request_arg))->getValueType()->isArrayTy())));

  const Value *base = canonicalMemoryBase(request_arg);

  if (const auto *gv = dyn_cast<GlobalVariable>(base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands(); ++i) {
          if (const auto *elem = dyn_cast<Constant>(array->getOperand(i))) {
            requests.push_back(elem->stripPointerCasts());
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast<AllocaInst>(base)) {
    if (context && context->getFunction() == alloca->getFunction()) {
      std::map<uint64_t, RequestID> same_block_requests;
      for (const Instruction &inst : *context->getParent()) {
        if (&inst == context) {
          break;
        }
        const auto *store = dyn_cast<StoreInst>(&inst);
        uint64_t index = 0;
        if (!store || !getIndexedStoreTarget(store, base, index)) {
          continue;
        }
        same_block_requests[index] =
            store->getValueOperand()->stripPointerCasts();
      }
      if (!same_block_requests.empty()) {
        for (const auto &entry : same_block_requests) {
          requests.push_back(entry.second);
        }
        return requests;
      }
    }

    std::map<uint64_t, std::set<RequestID>> indexed_requests;
    bool ambiguous_index = false;
    for (const Instruction &inst : instructions(alloca->getFunction())) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || (context && !mayDefinitionReach(store, context))) {
        continue;
      }
      uint64_t index = 0;
      if (!getIndexedStoreTarget(store, base, index)) {
        continue;
      }
      const Value *stored = store->getValueOperand()->stripPointerCasts();
      if (!stored) {
        ambiguous_index = true;
        continue;
      }
      indexed_requests[index].insert(stored);
    }
    if (ambiguous_index) {
      return {};
    }
    for (const auto &entry : indexed_requests) {
      if (entry.second.size() != 1) {
        return {};
      }
      requests.push_back(*entry.second.begin());
    }
  }

  if (requests.empty()) {
    if (likely_request_array) {
      return {};
    }
    requests.push_back(request_arg->stripPointerCasts());
  }
  return requests;
}

std::vector<int> MPIProcessModel::collectCompletedRequestIndices(
    const Value *indices_arg, size_t bound, const Instruction *context) const {
  std::set<int> completed;
  if (!indices_arg || bound == 0) {
    return {};
  }

  const Value *base = canonicalMemoryBase(indices_arg);

  if (const auto *gv = dyn_cast<GlobalVariable>(base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *array = dyn_cast<ConstantArray>(init)) {
        for (unsigned i = 0; i < array->getNumOperands(); ++i) {
          const auto *ci = dyn_cast<ConstantInt>(array->getOperand(i));
          if (!ci) {
            continue;
          }
          int index = ci->getSExtValue();
          if (index >= 0 && static_cast<size_t>(index) < bound) {
            completed.insert(index);
          }
        }
      }
    }
  } else if (const auto *alloca = dyn_cast<AllocaInst>(base)) {
    if (context && context->getFunction() == alloca->getFunction()) {
      std::map<uint64_t, int> same_block_values;
      for (const Instruction &inst : *context->getParent()) {
        if (&inst == context) {
          break;
        }
        const auto *store = dyn_cast<StoreInst>(&inst);
        const auto *stored_idx =
            store ? dyn_cast<ConstantInt>(store->getValueOperand()) : nullptr;
        uint64_t array_index = 0;
        if (!stored_idx || !getIndexedStoreTarget(store, base, array_index)) {
          continue;
        }
        int index = stored_idx->getSExtValue();
        if (index >= 0 && static_cast<size_t>(index) < bound) {
          same_block_values[array_index] = index;
        }
      }
      if (!same_block_values.empty()) {
        for (const auto &entry : same_block_values) {
          completed.insert(entry.second);
        }
        std::vector<int> result;
        result.reserve(completed.size());
        for (int index : completed) {
          result.push_back(index);
        }
        return result;
      }
    }

    std::map<uint64_t, std::set<int>> values_by_slot;
    for (const Instruction &inst : instructions(alloca->getFunction())) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || (context && !mayDefinitionReach(store, context))) {
        continue;
      }
      const auto *stored_idx = dyn_cast<ConstantInt>(store->getValueOperand());
      uint64_t array_index = 0;
      if (!stored_idx || !getIndexedStoreTarget(store, base, array_index)) {
        continue;
      }
      int index = stored_idx->getSExtValue();
      if (index >= 0 && static_cast<size_t>(index) < bound) {
        values_by_slot[array_index].insert(index);
      }
    }
    for (const auto &entry : values_by_slot) {
      if (entry.second.size() != 1) {
        return {};
      }
      completed.insert(*entry.second.begin());
    }
  }

  std::vector<int> result;
  result.reserve(completed.size());
  for (int index : completed) {
    result.push_back(index);
  }
  return result;
}

bool MPIProcessModel::tryReadScalarInt(const Value *scalar_arg, int &out,
                                       const Instruction *context) const {
  if (!scalar_arg) {
    return false;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(scalar_arg)) {
    out = ci->getSExtValue();
    return true;
  }

  const Value *base = canonicalMemoryBase(scalar_arg);

  std::set<int> seen_values;
  if (const auto *gv = dyn_cast<GlobalVariable>(base)) {
    if (const auto *init = gv->getInitializer()) {
      if (const auto *ci = dyn_cast<ConstantInt>(init)) {
        seen_values.insert(ci->getSExtValue());
      }
    }
  } else if (const auto *alloca = dyn_cast<AllocaInst>(base)) {
    if (context && context->getFunction() == alloca->getFunction()) {
      const StoreInst *same_block_store = nullptr;
      for (const Instruction &inst : *context->getParent()) {
        if (&inst == context) {
          break;
        }
        const auto *store = dyn_cast<StoreInst>(&inst);
        if (store && isDirectStoreToLocation(store, base)) {
          same_block_store = store;
        }
      }
      if (same_block_store) {
        if (const auto *stored =
                dyn_cast<ConstantInt>(same_block_store->getValueOperand())) {
          out = stored->getSExtValue();
          return true;
        }
        return false;
      }
    }

    const Function *parent = alloca->getFunction();
    if (!parent) {
      return false;
    }
    for (const Instruction &inst : instructions(parent)) {
      const auto *store = dyn_cast<StoreInst>(&inst);
      if (!store || (context && !mayDefinitionReach(store, context))) {
        continue;
      }
      if (!isDirectStoreToLocation(store, base)) {
        continue;
      }
      if (const auto *stored =
              dyn_cast<ConstantInt>(store->getValueOperand())) {
        seen_values.insert(stored->getSExtValue());
      } else {
        return false;
      }
    }
  }

  if (seen_values.size() != 1) {
    return false;
  }
  out = *seen_values.begin();
  return true;
}

void MPIProcessModel::buildSemanticEvents() {
  semantic_events_.clear();
  semantic_events_.reserve(all_operations_.size());

  for (size_t index = 0; index < all_operations_.size(); ++index) {
    const MPIOperation &op = all_operations_[index];
    MPIEvent event;
    event.operation_index = index;
    event.inst = op.inst;
    event.operation = &op;
    event.kind = classifySemanticEventKind(op);
    event.relation = op.semantic_relation;

    if (isCollectiveKind(op.kind)) {
      event.has_collective_semantics = true;
      event.collective.scope.communicator_class_id = op.communicator_class_id;
      event.collective.scope.communicator_subgroup_id =
          op.communicator_subgroup_id;
      event.collective.scope.participant_class_id = op.participant_class_id;
      event.collective.scope.protocol_class_id =
          op.collective_protocol_class_id;
      event.collective.type = op.td_type;
      event.collective.variant = op.collective_variant;
      event.collective.shape = op.collective_shape;
      event.collective.reachability = op.protocol_reachability;

      const auto *cb = dyn_cast<CallBase>(op.inst);
      switch (op.td_type) {
      case ThreadAPI::TD_MPI_BCAST:
        readConstCallArg(cb, 1, event.collective.count);
        readConstCallArg(cb, 2, event.collective.datatype);
        readConstCallArg(cb, 3, event.collective.root_rank);
        break;
      case ThreadAPI::TD_MPI_REDUCE:
        readConstCallArg(cb, 2, event.collective.count);
        readConstCallArg(cb, 3, event.collective.datatype);
        readConstCallArg(cb, 4, event.collective.reduction_op);
        readConstCallArg(cb, 5, event.collective.root_rank);
        break;
      case ThreadAPI::TD_MPI_GATHER:
      case ThreadAPI::TD_MPI_SCATTER:
        readConstCallArg(cb, 1, event.collective.count);
        readConstCallArg(cb, 2, event.collective.datatype);
        readConstCallArg(cb, 4, event.collective.recv_count);
        readConstCallArg(cb, 5, event.collective.recv_datatype);
        readConstCallArg(cb, 6, event.collective.root_rank);
        if (cb && cb->arg_size() > 3) {
          event.collective.in_place =
              cb->getArgOperand(0) == cb->getArgOperand(3);
        }
        break;
      case ThreadAPI::TD_MPI_ALLGATHER:
      case ThreadAPI::TD_MPI_ALLTOALL:
        readConstCallArg(cb, 1, event.collective.count);
        readConstCallArg(cb, 2, event.collective.datatype);
        readConstCallArg(cb, 4, event.collective.recv_count);
        readConstCallArg(cb, 5, event.collective.recv_datatype);
        break;
      case ThreadAPI::TD_MPI_ALLREDUCE:
        readConstCallArg(cb, 2, event.collective.count);
        readConstCallArg(cb, 3, event.collective.datatype);
        readConstCallArg(cb, 4, event.collective.reduction_op);
        break;
      case ThreadAPI::TD_MPI_REDUCE_SCATTER:
      case ThreadAPI::TD_MPI_SCAN:
        readConstCallArg(cb, 2, event.collective.datatype);
        readConstCallArg(cb, 3, event.collective.reduction_op);
        break;
      default:
        break;
      }
    }

    if (event.kind == MPIEventKind::PointToPoint) {
      event.has_point_to_point_semantics = true;
      event.point_to_point.is_send = op.kind == MPIOpKind::SEND_BLOCKING ||
                                     op.kind == MPIOpKind::SEND_NONBLOCKING;
      event.point_to_point.is_recv = op.kind == MPIOpKind::RECV_BLOCKING ||
                                     op.kind == MPIOpKind::RECV_NONBLOCKING;
      event.point_to_point.is_probe = op.kind == MPIOpKind::PROBE_BLOCKING ||
                                      op.kind == MPIOpKind::PROBE_NONBLOCKING;
      event.point_to_point.send_mode = op.send_mode;
      event.point_to_point.blocking_mode = op.blocking_mode;
      event.point_to_point.participant_class_id = op.participant_class_id;
      if (event.point_to_point.is_send) {
        event.point_to_point.peer_rank = op.dest_rank;
        event.point_to_point.peer_rank_min = op.dest_rank_min;
        event.point_to_point.peer_rank_max = op.dest_rank_max;
      } else {
        event.point_to_point.peer_rank = op.source_rank;
        event.point_to_point.peer_rank_min = op.source_rank_min;
        event.point_to_point.peer_rank_max = op.source_rank_max;
      }
      event.point_to_point.local_rank =
          op.process_rank.kind == MPI::RankExpr::Concrete
              ? op.process_rank.concrete_value
              : -1;
      event.point_to_point.tag = op.tag;
      event.point_to_point.communicator_class_id = op.communicator_class_id;
      event.point_to_point.reachability = op.protocol_reachability;
      event.point_to_point.datatype_size = op.datatype_size;
    }

    if (event.kind == MPIEventKind::RMA) {
      event.has_rma_semantics = true;
      event.rma.window = op.window;
      event.rma.is_window_lifecycle = op.kind == MPIOpKind::RMA_WINDOW;
      event.rma.is_data_operation = op.kind == MPIOpKind::RMA_DATA;
      event.rma.is_sync_operation = op.kind == MPIOpKind::RMA_SYNC;
      event.rma.access_kind = op.rma_access_kind;
      event.rma.sync_kind = op.rma_sync_kind;
      event.rma.participant_class_id = op.participant_class_id;
      event.rma.target_rank = op.target_rank;
      event.rma.target_rank_min = op.target_rank_min;
      event.rma.target_rank_max = op.target_rank_max;
      event.rma.target_disp = op.target_disp;
      event.rma.byte_length = op.byte_length;
      event.rma.epoch_kind = op.rma_epoch_kind;
      event.rma.local_completion_only = op.rma_local_completion_only;
      event.rma.lock_all = op.rma_lock_all;
    }

    if (isNonBlockingRequestKind(op.kind) || op.kind == MPIOpKind::WAIT ||
        op.kind == MPIOpKind::TEST ||
        (op.kind == MPIOpKind::COMM_MANAGEMENT &&
         op.request_lifecycle_issue_nonblocking) ||
        op.kind == MPIOpKind::REQUEST_MANAGEMENT) {
      event.has_request_semantics = true;
      event.request.arity = op.request_arity;
      if (isNonBlockingRequestKind(op.kind) && op.request) {
        event.request.action = MPIRequestActionKind::IssueNonBlocking;
        event.request.requests.push_back(op.request);
      } else if (op.kind == MPIOpKind::COMM_MANAGEMENT &&
                 op.request_lifecycle_issue_nonblocking && op.request) {
        event.request.action = MPIRequestActionKind::IssueNonBlocking;
        event.request.requests.push_back(op.request);
      } else if (op.kind == MPIOpKind::REQUEST_MANAGEMENT && op.request) {
        event.request.requests = collectRequestOperands(op.request, op.inst);
        switch (op.td_type) {
        case ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT:
        case ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT:
          event.request.action = MPIRequestActionKind::CreatePersistent;
          if (event.request.requests.empty()) {
            event.request.requests.push_back(op.request);
          }
          break;
        case ThreadAPI::TD_MPI_REQUEST_START:
          event.request.action = MPIRequestActionKind::ActivatePersistent;
          break;
        case ThreadAPI::TD_MPI_REQUEST_FREE:
          event.request.action = MPIRequestActionKind::Free;
          break;
        case ThreadAPI::TD_MPI_CANCEL:
          event.request.action = MPIRequestActionKind::Cancel;
          break;
        default:
          event.request.action = MPIRequestActionKind::Observe;
          break;
        }
      } else if (op.request) {
        event.request.requests = collectRequestOperands(op.request, op.inst);
        event.request.action = MPIRequestActionKind::Observe;

        const auto *cb = dyn_cast<CallBase>(op.inst);
        switch (op.td_type) {
        case ThreadAPI::TD_MPI_WAIT:
        case ThreadAPI::TD_MPI_WAITALL:
          event.request.action = MPIRequestActionKind::CompleteMust;
          break;
        case ThreadAPI::TD_MPI_TEST:
          if (cb && cb->arg_size() >= 2) {
            int flag = 0;
            if (tryReadScalarInt(cb->getArgOperand(1), flag, op.inst)) {
              event.request.completion_flag_known = true;
              event.request.completion_flag = flag != 0;
            }
          }
          break;
        case ThreadAPI::TD_MPI_TESTALL:
          if (cb && cb->arg_size() >= 3) {
            int flag = 0;
            if (tryReadScalarInt(cb->getArgOperand(2), flag, op.inst)) {
              event.request.completion_flag_known = true;
              event.request.completion_flag = flag != 0;
            }
          }
          break;
        case ThreadAPI::TD_MPI_WAITANY:
          if (cb && cb->arg_size() >= 3) {
            int selected = -1;
            if (tryReadScalarInt(cb->getArgOperand(2), selected, op.inst)) {
              event.request.completed_indices.push_back(selected);
            }
          }
          break;
        case ThreadAPI::TD_MPI_TESTANY:
          if (cb && cb->arg_size() >= 4) {
            int flag = 0;
            if (tryReadScalarInt(cb->getArgOperand(3), flag, op.inst)) {
              event.request.completion_flag_known = true;
              event.request.completion_flag = flag != 0;
            }
            int selected = -1;
            if (tryReadScalarInt(cb->getArgOperand(2), selected, op.inst)) {
              event.request.completed_indices.push_back(selected);
            }
          }
          break;
        case ThreadAPI::TD_MPI_WAITSOME:
          if (cb && cb->arg_size() >= 4) {
            event.request.completed_indices = collectCompletedRequestIndices(
                cb->getArgOperand(3), event.request.requests.size(), op.inst);
          }
          break;
        case ThreadAPI::TD_MPI_TESTSOME:
          if (cb && cb->arg_size() >= 3) {
            int outcount = 0;
            if (tryReadScalarInt(cb->getArgOperand(2), outcount, op.inst)) {
              event.request.outcount_known = true;
              event.request.outcount = outcount;
            }
          }
          if (cb && cb->arg_size() >= 4) {
            event.request.completed_indices = collectCompletedRequestIndices(
                cb->getArgOperand(3), event.request.requests.size(), op.inst);
          }
          break;
        default:
          break;
        }
      }
    }

    semantic_events_.push_back(std::move(event));
  }
}

void MPIProcessModel::buildPointToPointObligations() {
  point_to_point_obligations_.clear();
  channel_endpoint_obligations_.clear();
  channel_obligations_.clear();
  struct CandidatePair {
    size_t sender_endpoint_index = 0;
    size_t receiver_endpoint_index = 0;
    size_t send_operation_index = 0;
    size_t recv_operation_index = 0;
    size_t channel_class_id = 0;
    MPICommunicationMatch base_match = MPICommunicationMatch::NoMatch;
  };

  size_t next_endpoint_id = 1;
  std::vector<size_t> send_endpoint_indices;
  std::vector<size_t> recv_endpoint_indices;
  for (size_t op_index = 0; op_index < all_operations_.size(); ++op_index) {
    const MPIOperation &op = all_operations_[op_index];
    if (op.matched_message) {
      continue;
    }
    if (!isSendOperationKind(op.kind) && !isRecvOperationKind(op.kind)) {
      continue;
    }
    MPIChannelEndpointObligation endpoint;
    endpoint.obligation_id = next_endpoint_id++;
    endpoint.operation_index = op_index;
    endpoint.inst = op.inst;
    endpoint.communicator_class_id = op.communicator_class_id;
    endpoint.endpoint_kind = isSendOperationKind(op.kind)
                                 ? MPIChannelEndpointKind::Send
                                 : MPIChannelEndpointKind::Receive;
    endpoint.participants = op.participant_set;
    endpoint.peer_rank = isSendOperationKind(op.kind) ? op.dest_rank : op.source_rank;
    endpoint.peer_rank_min =
        isSendOperationKind(op.kind) ? op.dest_rank_min : op.source_rank_min;
    endpoint.peer_rank_max =
        isSendOperationKind(op.kind) ? op.dest_rank_max : op.source_rank_max;
    endpoint.tag = op.tag;
    endpoint.tag_class =
        (isMPIWildcardValue(op.tag) || op.tag < 0) ? MPITagClassKind::Wildcard
                                                   : MPITagClassKind::Exact;
    endpoint.datatype_size = op.datatype_size;
    endpoint.send_mode = op.send_mode;
    endpoint.request = op.request;
    endpoint.blocking = isBlockingPointToPointKind(op.kind);
    endpoint.communicator_resolved =
        op.communicator_class_id != 0 && !op.participant_set.unknown;
    channel_endpoint_obligations_.push_back(endpoint);
    size_t endpoint_index = channel_endpoint_obligations_.size() - 1;
    if (isSendOperationKind(op.kind)) {
      send_endpoint_indices.push_back(endpoint_index);
    } else {
      recv_endpoint_indices.push_back(endpoint_index);
    }
  }

  std::unordered_map<RequestID, size_t> request_to_set_id;
  for (const MPIRequestSetFact &request_set : request_set_facts_) {
    for (RequestID request : request_set.requests) {
      if (request && request_to_set_id.count(request) == 0) {
        request_to_set_id[request] = request_set.request_set_id;
      }
    }
  }

  std::map<std::string, size_t> channel_ids;
  size_t next_channel_id = 1;
  std::vector<CandidatePair> candidate_pairs;
  std::unordered_map<size_t, std::set<size_t>> endpoint_channel_classes;

  for (size_t sender_endpoint_index : send_endpoint_indices) {
    const MPIOperation &send =
        all_operations_[channel_endpoint_obligations_[sender_endpoint_index].operation_index];
    for (size_t receiver_endpoint_index : recv_endpoint_indices) {
      const MPIOperation &recv =
          all_operations_[channel_endpoint_obligations_[receiver_endpoint_index].operation_index];
      if (!isPotentialChannelPair(send, recv, &module_)) {
        continue;
      }
      MPICommunicationMatch base_match = classifyCommunicationMatch(send, recv);
      if (base_match == MPICommunicationMatch::NoMatch) {
        continue;
      }

      MPIChannelEndpointObligation &sender_endpoint =
          channel_endpoint_obligations_[sender_endpoint_index];
      MPIChannelEndpointObligation &receiver_endpoint =
          channel_endpoint_obligations_[receiver_endpoint_index];
      sender_endpoint.candidate_ids.push_back(receiver_endpoint.obligation_id);
      receiver_endpoint.candidate_ids.push_back(sender_endpoint.obligation_id);

      const int tag_key = sender_endpoint.tag_class == MPITagClassKind::Exact &&
                                  receiver_endpoint.tag_class == MPITagClassKind::Exact
                              ? (send.tag >= 0 ? send.tag : recv.tag)
                              : -1;
      std::string channel_key =
          std::to_string(send.communicator_class_id != 0
                             ? send.communicator_class_id
                             : recv.communicator_class_id) +
          ":" + send.participant_set.toKey() + ":" + recv.participant_set.toKey() +
          ":" + std::to_string(tag_key) + ":" +
          std::to_string(std::max(send.datatype_size, recv.datatype_size)) + ":" +
          std::to_string(static_cast<int>(send.send_mode));
      auto id_it = channel_ids.emplace(channel_key, next_channel_id).first;
      if (id_it->second == next_channel_id) {
        ++next_channel_id;
      }
      endpoint_channel_classes[sender_endpoint_index].insert(id_it->second);
      endpoint_channel_classes[receiver_endpoint_index].insert(id_it->second);
      candidate_pairs.push_back({sender_endpoint_index, receiver_endpoint_index,
                                 sender_endpoint.operation_index,
                                 receiver_endpoint.operation_index, id_it->second,
                                 base_match});
    }
  }

  for (size_t endpoint_index = 0; endpoint_index < channel_endpoint_obligations_.size();
       ++endpoint_index) {
    MPIChannelEndpointObligation &endpoint = channel_endpoint_obligations_[endpoint_index];
    auto channel_it = endpoint_channel_classes.find(endpoint_index);
    if (channel_it != endpoint_channel_classes.end() && channel_it->second.size() == 1) {
      endpoint.channel_class_id = *channel_it->second.begin();
      all_operations_[endpoint.operation_index].channel_class_id = endpoint.channel_class_id;
    } else {
      all_operations_[endpoint.operation_index].channel_class_id = 0;
    }
  }

  for (const CandidatePair &candidate : candidate_pairs) {
    MPIOperation &send = all_operations_[candidate.send_operation_index];
    MPIOperation &recv = all_operations_[candidate.recv_operation_index];
    MPIChannelEndpointObligation &sender_endpoint =
        channel_endpoint_obligations_[candidate.sender_endpoint_index];
    MPIChannelEndpointObligation &receiver_endpoint =
        channel_endpoint_obligations_[candidate.receiver_endpoint_index];

    MPICommunicationMatch match = candidate.base_match;
    if (match == MPICommunicationMatch::MustMatch &&
        (sender_endpoint.candidate_ids.size() != 1 ||
         receiver_endpoint.candidate_ids.size() != 1)) {
      match = MPICommunicationMatch::MayMatch;
    }

    MPIPointToPointObligation obligation;
    obligation.lhs_operation_index = candidate.send_operation_index;
    obligation.rhs_operation_index = candidate.recv_operation_index;
    obligation.lhs_inst = send.inst;
    obligation.rhs_inst = recv.inst;
    obligation.communicator_class_id = send.communicator_class_id != 0
                                           ? send.communicator_class_id
                                           : recv.communicator_class_id;
    obligation.send_rank = send.dest_rank;
    obligation.recv_rank = recv.source_rank;
    obligation.tag = send.tag >= 0 ? send.tag : recv.tag;
    obligation.send_participant_class_id = send.participant_class_id;
    obligation.recv_participant_class_id = recv.participant_class_id;
    obligation.send_datatype_size = send.datatype_size;
    obligation.recv_datatype_size = recv.datatype_size;
    obligation.send_mode = send.send_mode;
    obligation.send_is_blocking = send.kind == MPIOpKind::SEND_BLOCKING;
    obligation.recv_is_blocking = recv.kind == MPIOpKind::RECV_BLOCKING;
    switch (match) {
    case MPICommunicationMatch::MustMatch:
      obligation.proof = MPIMatchProofKind::MustMatch;
      obligation.relation.kind = concurrency::RelationKind::MustHappenBefore;
      obligation.relation.proof = concurrency::ProofStrength::Must;
      obligation.relation.reason = "mpi_point_to_point_match_obligation";
      break;
    case MPICommunicationMatch::MayMatch:
      obligation.proof = MPIMatchProofKind::MayMatch;
      obligation.relation.kind = concurrency::RelationKind::MayHappenBefore;
      obligation.relation.proof = concurrency::ProofStrength::May;
      obligation.relation.reason = "mpi_point_to_point_match_obligation";
      break;
    case MPICommunicationMatch::Unknown:
      obligation.proof = MPIMatchProofKind::Unknown;
      obligation.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
      obligation.relation.proof = concurrency::ProofStrength::Unknown;
      obligation.relation.reason = "mpi_point_to_point_match_obligation";
      break;
    case MPICommunicationMatch::NoMatch:
      obligation.proof = MPIMatchProofKind::NoMatch;
      break;
    }
    point_to_point_obligations_.push_back(obligation);

    MPIChannelObligation channel;
    channel.channel_class_id = candidate.channel_class_id;
    channel.sender_obligation_id = sender_endpoint.obligation_id;
    channel.receiver_obligation_id = receiver_endpoint.obligation_id;
    channel.request_set_id = sender_endpoint.request && request_to_set_id.count(sender_endpoint.request)
                                 ? request_to_set_id[sender_endpoint.request]
                                 : (receiver_endpoint.request &&
                                            request_to_set_id.count(receiver_endpoint.request)
                                        ? request_to_set_id[receiver_endpoint.request]
                                        : 0);
    channel.lhs_operation_index = candidate.send_operation_index;
    channel.rhs_operation_index = candidate.recv_operation_index;
    channel.sender_operation_index = candidate.send_operation_index;
    channel.receiver_operation_index = candidate.recv_operation_index;
    channel.lhs_inst = send.inst;
    channel.rhs_inst = recv.inst;
    channel.sender_inst = send.inst;
    channel.receiver_inst = recv.inst;
    channel.communicator_class_id = obligation.communicator_class_id;
    channel.sender_set = send.participant_set;
    channel.receiver_set = recv.participant_set;
    channel.tag = obligation.tag;
    channel.tag_class = sender_endpoint.tag_class == MPITagClassKind::Exact &&
                                receiver_endpoint.tag_class == MPITagClassKind::Exact
                            ? MPITagClassKind::Exact
                            : MPITagClassKind::Wildcard;
    channel.send_datatype_size = send.datatype_size;
    channel.recv_datatype_size = recv.datatype_size;
    channel.send_mode = send.send_mode;
    channel.request = send.request ? send.request : recv.request;
    channel.sender_request = send.request;
    channel.receiver_request = recv.request;
    channel.send_is_blocking = obligation.send_is_blocking;
    channel.recv_is_blocking = obligation.recv_is_blocking;
    channel.proof = match;
    channel.proof_source =
        match == MPICommunicationMatch::MustMatch
            ? "unique_channel_candidate"
            : (match == MPICommunicationMatch::MayMatch &&
                       candidate.base_match == MPICommunicationMatch::MustMatch
                   ? "mpi_channel_candidate_nonunique"
                   : (match == MPICommunicationMatch::Unknown
                          ? "mpi_channel_identity_unresolved"
                          : "compatible_channel_candidate"));
    channel.relation = obligation.relation;
    if (match == MPICommunicationMatch::MustMatch ||
        match == MPICommunicationMatch::MayMatch) {
      channel.relation.kind = concurrency::RelationKind::MatchedCommunication;
      channel.relation.reason = channel.proof_source;
    }

    if (channel.request) {
      auto isBlockingWaitInst = [&](const Instruction *inst) {
        if (!inst) {
          return false;
        }
        auto op_it = std::find_if(
            all_operations_.begin(), all_operations_.end(),
            [&](const MPIOperation &op) { return op.inst == inst; });
        return op_it != all_operations_.end() && op_it->kind == MPIOpKind::WAIT;
      };
      auto markFromRequest = [&](RequestID request) {
        if (!request) {
          return false;
        }
        auto summary_it = request_state_summaries_.find(request);
        if (summary_it == request_state_summaries_.end()) {
          return false;
        }
        if (!isResolvedRequestState(summary_it->second.state)) {
          return false;
        }
        if (isBlockingWaitInst(summary_it->second.last_transition_inst)) {
          return false;
        }
        channel.discharged = true;
        channel.discharge_inst = summary_it->second.last_transition_inst;
        return true;
      };
      if (!markFromRequest(channel.sender_request)) {
        markFromRequest(channel.receiver_request);
      }
    }

    channel_obligations_.push_back(channel);

    if (match == MPICommunicationMatch::MayMatch ||
        match == MPICommunicationMatch::Unknown) {
      MPIModelGap gap;
      gap.domain = MPIModelGapDomain::PointToPoint;
      gap.inst = send.inst;
      gap.communicator = send.communicator;
      gap.communicator_class_id = obligation.communicator_class_id;
      gap.subgroup_id = send.communicator_subgroup_id;
      gap.participant_class_id = send.participant_class_id;
      gap.relation = channel.relation;
      gap.code = match == MPICommunicationMatch::Unknown
                     ? "mpi_channel_identity_unresolved"
                     : "mpi_channel_candidate_nonunique";
      gap.detail = send.participant_set.toKey() + " -> " +
                   recv.participant_set.toKey();
      model_gaps_.push_back(gap);
    }
  }

  for (MPIOperation &op : all_operations_) {
    if (!op.request) {
      continue;
    }
    auto summary_it = request_state_summaries_.find(op.request);
    if (summary_it != request_state_summaries_.end()) {
      summary_it->second.channel_class_id = op.channel_class_id;
    }
  }
  for (MPIRequestSetFact &request_set : request_set_facts_) {
    for (RequestID request : request_set.requests) {
      auto summary_it = request_state_summaries_.find(request);
      if (summary_it != request_state_summaries_.end() &&
          summary_it->second.channel_class_id != 0) {
        request_set.channel_class_id = summary_it->second.channel_class_id;
        break;
      }
    }
  }
}

void MPIProcessModel::analyzeRequestStateDomain() {
  non_blocking_ops_.clear();
  request_state_summaries_.clear();
  request_set_facts_.clear();
  size_t next_request_set_id = 1;

  auto ensureSummary = [&](RequestID request, const MPIOperation &op,
                           bool persistent) -> MPIRequestStateSummary & {
    MPIRequestStateSummary &summary = request_state_summaries_[request];
    if (!summary.request) {
      summary.request = request;
      summary.origin_inst = op.inst;
      summary.peer_rank = op.kind == MPIOpKind::SEND_NONBLOCKING
                              ? op.dest_rank
                              : op.source_rank;
      summary.tag = op.tag;
      summary.communicator = op.communicator;
      summary.send_mode = op.send_mode;
      summary.state =
          persistent ? MPIRequestState::Created : MPIRequestState::Pending;
      summary.is_collective = op.kind == MPIOpKind::BARRIER_NONBLOCKING ||
                              op.kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
      summary.provenance = persistent ? "persistent-template" : "request-issue";
    }
    summary.is_persistent = summary.is_persistent || persistent;
    return summary;
  };

  auto recordTransition =
      [&](MPIRequestStateSummary &summary, MPIRequestActionKind action,
          MPIRequestState next_state, const Instruction *inst) {
        MPIRequestTransition transition;
        transition.action = action;
        transition.from_state = summary.state;
        transition.to_state = joinRequestState(summary.state, next_state);
        transition.inst = inst;
        summary.state = transition.to_state;
        summary.last_transition_inst = inst;
        summary.history.push_back(transition);
      };

  auto emitRequestSetFact = [&](const MPIEvent &event, const MPIOperation &op,
                                MPIRequestState state_override,
                                MPIRequestCompletionScopeKind scope_override,
                                llvm::StringRef provenance) {
    if (event.request.requests.empty()) {
      return size_t(0);
    }
    MPIRequestSetFact fact;
    fact.request_set_id = next_request_set_id++;
    fact.communicator_class_id = op.communicator_class_id;
    fact.requests = event.request.requests;
    fact.arity = event.request.arity;
    fact.kind = classifyRequestSetKind(op, event.request.action ==
                                               MPIRequestActionKind::CreatePersistent ||
                                           event.request.action ==
                                               MPIRequestActionKind::ActivatePersistent);
    fact.state = state_override;
    fact.completion_scope = scope_override;
    fact.origin_inst = op.request ? op.inst : nullptr;
    fact.transition_inst = op.inst;
    fact.provenance = provenance.str();
    fact.relation.kind = state_override == MPIRequestState::MustComplete
                             ? concurrency::RelationKind::MPIRequestCompletion
                             : concurrency::RelationKind::UnknownDueToModelGap;
    fact.relation.proof = state_override == MPIRequestState::MustComplete
                              ? concurrency::ProofStrength::Must
                              : (state_override == MPIRequestState::MayComplete
                                     ? concurrency::ProofStrength::May
                                     : concurrency::ProofStrength::Unknown);
    fact.relation.reason = provenance.str();
    request_set_facts_.push_back(fact);
    return fact.request_set_id;
  };

  auto markAllMayComplete = [&](const MPIEvent &event, const MPIOperation &op) {
    size_t request_set_id = emitRequestSetFact(
        event, op, MPIRequestState::MayComplete,
        completionScopeForAction(op, event), "mpi_request_set_may_complete");
    for (RequestID request : event.request.requests) {
      MPIRequestStateSummary &summary = ensureSummary(request, op, false);
      recordTransition(summary, MPIRequestActionKind::CompleteMay,
                       MPIRequestState::MayComplete, op.inst);
      summary.request_set_id = request_set_id;
      summary.completion_scope = completionScopeForAction(op, event);
      summary.provenance = "mpi_request_set_may_complete";
    }
  };

  auto recordCompletionModelGap = [&](const MPIOperation &op,
                                      llvm::StringRef code,
                                      llvm::StringRef detail) {
    MPIModelGap gap;
    gap.domain = MPIModelGapDomain::Completion;
    gap.inst = op.inst;
    gap.communicator = op.communicator;
    gap.communicator_class_id = op.communicator_class_id;
    gap.participant_class_id = op.participant_class_id;
    gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
    gap.relation.proof = concurrency::ProofStrength::Unknown;
    gap.relation.reason = code.str();
    gap.code = code.str();
    gap.detail = detail.str();
    model_gaps_.push_back(gap);
  };

  for (const MPIEvent &event : semantic_events_) {
    if (!event.has_request_semantics) {
      continue;
    }
    const MPIOperation &op = all_operations_[event.operation_index];
    if (event.request.requests.empty()) {
      if (op.request && op.request_arity == MPIRequestArity::Array) {
        recordCompletionModelGap(op, "mpi_request_storage_escaped",
                                 "MPI request storage or alias set unresolved");
      }
      continue;
    }

    switch (event.request.action) {
    case MPIRequestActionKind::IssueNonBlocking:
      {
        size_t request_set_id = emitRequestSetFact(
            event, op, MPIRequestState::Active,
            completionScopeForAction(op, event), "mpi_request_set_issue");
      for (RequestID request : event.request.requests) {
        MPIRequestStateSummary &summary = ensureSummary(request, op, false);
        summary.activation_inst = op.inst;
        recordTransition(summary, MPIRequestActionKind::IssueNonBlocking,
                         MPIRequestState::Active, op.inst);
        summary.request_set_id = request_set_id;
        summary.completion_scope = completionScopeForAction(op, event);
        summary.provenance = "mpi_request_set_issue";
      }
      }
      break;
    case MPIRequestActionKind::CreatePersistent:
      {
        size_t request_set_id = emitRequestSetFact(
            event, op, MPIRequestState::Created,
            completionScopeForAction(op, event), "mpi_request_set_create");
      for (RequestID request : event.request.requests) {
        MPIRequestStateSummary &summary = ensureSummary(request, op, true);
        recordTransition(summary, MPIRequestActionKind::CreatePersistent,
                         MPIRequestState::Created, op.inst);
        summary.request_set_id = request_set_id;
        summary.completion_scope = completionScopeForAction(op, event);
        summary.provenance = "mpi_request_set_create";
      }
      }
      break;
    case MPIRequestActionKind::ActivatePersistent:
      {
        size_t request_set_id = emitRequestSetFact(
            event, op, MPIRequestState::Active,
            completionScopeForAction(op, event), "mpi_request_set_activate");
      for (RequestID request : event.request.requests) {
        MPIRequestStateSummary &summary = ensureSummary(request, op, true);
        summary.activation_inst = op.inst;
        recordTransition(summary, MPIRequestActionKind::ActivatePersistent,
                         MPIRequestState::Active, op.inst);
        summary.request_set_id = request_set_id;
        summary.completion_scope = completionScopeForAction(op, event);
        summary.provenance = "mpi_request_set_activate";
      }
      }
      break;
    case MPIRequestActionKind::Free:
      {
        size_t request_set_id = emitRequestSetFact(
            event, op, MPIRequestState::Freed,
            completionScopeForAction(op, event), "mpi_request_set_free");
      for (RequestID request : event.request.requests) {
        MPIRequestStateSummary &summary = ensureSummary(request, op, false);
        recordTransition(summary, MPIRequestActionKind::Free,
                         MPIRequestState::Freed, op.inst);
        summary.request_set_id = request_set_id;
        summary.completion_scope = completionScopeForAction(op, event);
        summary.provenance = "mpi_request_set_free";
      }
      }
      break;
    case MPIRequestActionKind::Cancel:
      {
        size_t request_set_id = emitRequestSetFact(
            event, op, MPIRequestState::Canceled,
            completionScopeForAction(op, event), "mpi_request_set_cancel");
      for (RequestID request : event.request.requests) {
        MPIRequestStateSummary &summary = ensureSummary(request, op, false);
        recordTransition(summary, MPIRequestActionKind::Cancel,
                         MPIRequestState::Terminal, op.inst);
        summary.request_set_id = request_set_id;
        summary.completion_scope = completionScopeForAction(op, event);
        summary.provenance = "mpi_request_set_cancel";
      }
      }
      break;
    case MPIRequestActionKind::CompleteMust:
      {
        size_t request_set_id = emitRequestSetFact(
            event, op, MPIRequestState::MustComplete,
            completionScopeForAction(op, event), "mpi_request_set_complete");
      for (RequestID request : event.request.requests) {
        MPIRequestStateSummary &summary = ensureSummary(request, op, false);
        recordTransition(summary, MPIRequestActionKind::CompleteMust,
                         MPIRequestState::MustComplete, op.inst);
        summary.request_set_id = request_set_id;
        summary.completion_scope = completionScopeForAction(op, event);
        summary.provenance = "mpi_request_set_complete";
      }
      }
      break;
    case MPIRequestActionKind::Observe:
      switch (op.td_type) {
      case ThreadAPI::TD_MPI_TEST:
        if (!event.request.completion_flag_known) {
          deferred_lowering_stats_["unknown_flag_value"]++;
          deferred_lowering_stats_["test_unknown_flag"]++;
          recordCompletionModelGap(op, "mpi_request_completion_flag_unresolved",
                                   "MPI_Test completion flag unresolved");
          markAllMayComplete(event, op);
          break;
        }
        if (event.request.completion_flag) {
          size_t request_set_id = emitRequestSetFact(
              event, op, MPIRequestState::MustComplete,
              completionScopeForAction(op, event), "mpi_request_set_test");
          RequestID request = event.request.requests.front();
          MPIRequestStateSummary &summary = ensureSummary(request, op, false);
          recordTransition(summary, MPIRequestActionKind::CompleteMust,
                           MPIRequestState::MustComplete, op.inst);
          summary.request_set_id = request_set_id;
          summary.completion_scope = completionScopeForAction(op, event);
          summary.provenance = "mpi_request_set_test";
        }
        break;
      case ThreadAPI::TD_MPI_TESTALL:
        if (!event.request.completion_flag_known) {
          deferred_lowering_stats_["testall_unknown_flag"]++;
          recordCompletionModelGap(op, "mpi_request_completion_flag_unresolved",
                                   "MPI_Testall completion flag unresolved");
          markAllMayComplete(event, op);
          break;
        }
        if (event.request.completion_flag) {
          size_t request_set_id = emitRequestSetFact(
              event, op, MPIRequestState::MustComplete,
              completionScopeForAction(op, event), "mpi_request_set_testall");
          for (RequestID request : event.request.requests) {
            MPIRequestStateSummary &summary = ensureSummary(request, op, false);
            recordTransition(summary, MPIRequestActionKind::CompleteMust,
                             MPIRequestState::MustComplete, op.inst);
            summary.request_set_id = request_set_id;
            summary.completion_scope = completionScopeForAction(op, event);
            summary.provenance = "mpi_request_set_testall";
          }
        }
        break;
      case ThreadAPI::TD_MPI_WAITANY:
        if (event.request.completed_indices.empty()) {
          deferred_lowering_stats_["waitany_unknown_index"]++;
          recordCompletionModelGap(op, "mpi_request_completion_index_unresolved",
                                   "MPI_Waitany selected index unresolved");
          markAllMayComplete(event, op);
          break;
        }
        if (event.request.completed_indices.front() >= 0 &&
            static_cast<size_t>(event.request.completed_indices.front()) <
                event.request.requests.size()) {
          size_t request_set_id = emitRequestSetFact(
              event, op, MPIRequestState::MustComplete,
              completionScopeForAction(op, event), "mpi_request_set_waitany");
          RequestID request = event.request.requests[static_cast<size_t>(
              event.request.completed_indices.front())];
          MPIRequestStateSummary &summary = ensureSummary(request, op, false);
          recordTransition(summary, MPIRequestActionKind::CompleteMust,
                           MPIRequestState::MustComplete, op.inst);
          summary.request_set_id = request_set_id;
          summary.completion_scope = completionScopeForAction(op, event);
          summary.provenance = "mpi_request_set_waitany";
        }
        break;
      case ThreadAPI::TD_MPI_TESTANY:
        if (!event.request.completion_flag_known) {
          deferred_lowering_stats_["unknown_flag_value"]++;
          deferred_lowering_stats_["testany_unknown_flag"]++;
          recordCompletionModelGap(op, "mpi_request_completion_flag_unresolved",
                                   "MPI_Testany completion flag unresolved");
        }
        if (event.request.completion_flag &&
            !event.request.completed_indices.empty() &&
            event.request.completed_indices.front() >= 0 &&
            static_cast<size_t>(event.request.completed_indices.front()) <
                event.request.requests.size()) {
          size_t request_set_id = emitRequestSetFact(
              event, op, MPIRequestState::MustComplete,
              completionScopeForAction(op, event), "mpi_request_set_testany");
          RequestID request = event.request.requests[static_cast<size_t>(
              event.request.completed_indices.front())];
          MPIRequestStateSummary &summary = ensureSummary(request, op, false);
          recordTransition(summary, MPIRequestActionKind::CompleteMust,
                           MPIRequestState::MustComplete, op.inst);
          summary.request_set_id = request_set_id;
          summary.completion_scope = completionScopeForAction(op, event);
          summary.provenance = "mpi_request_set_testany";
        } else if (event.request.completion_flag ||
                   !event.request.completion_flag_known) {
          if (event.request.completed_indices.empty()) {
            deferred_lowering_stats_["unknown_completed_index_set"]++;
            deferred_lowering_stats_["testany_unknown_index"]++;
            recordCompletionModelGap(op, "mpi_request_completion_index_unresolved",
                                     "MPI_Testany selected index unresolved");
          }
          markAllMayComplete(event, op);
        }
        break;
      case ThreadAPI::TD_MPI_WAITSOME:
        if (event.request.completed_indices.empty()) {
          deferred_lowering_stats_["waitsome_unknown_indices"]++;
          recordCompletionModelGap(op, "mpi_request_completion_subset_unresolved",
                                   "MPI_Waitsome completion set unresolved");
          markAllMayComplete(event, op);
          break;
        }
        {
          size_t request_set_id = emitRequestSetFact(
              event, op, MPIRequestState::MustComplete,
              completionScopeForAction(op, event), "mpi_request_set_waitsome");
        for (int index : event.request.completed_indices) {
          if (index < 0 ||
              static_cast<size_t>(index) >= event.request.requests.size()) {
            continue;
          }
          RequestID request =
              event.request.requests[static_cast<size_t>(index)];
          MPIRequestStateSummary &summary = ensureSummary(request, op, false);
          recordTransition(summary, MPIRequestActionKind::CompleteMust,
                           MPIRequestState::MustComplete, op.inst);
            summary.request_set_id = request_set_id;
            summary.completion_scope = completionScopeForAction(op, event);
            summary.provenance = "mpi_request_set_waitsome";
        }
        }
        break;
      case ThreadAPI::TD_MPI_TESTSOME:
        if (!event.request.outcount_known) {
          deferred_lowering_stats_["testsome_unknown_outcount"]++;
          recordCompletionModelGap(op, "mpi_request_completion_subset_unresolved",
                                   "MPI_Testsome outcount unresolved");
          markAllMayComplete(event, op);
          break;
        }
        if (event.request.outcount <= 0) {
          break;
        }
        if (event.request.completed_indices.empty()) {
          deferred_lowering_stats_["testsome_unknown_indices"]++;
          recordCompletionModelGap(op, "mpi_request_completion_subset_unresolved",
                                   "MPI_Testsome completion set unresolved");
          markAllMayComplete(event, op);
          break;
        }
        {
          size_t request_set_id = emitRequestSetFact(
              event, op, MPIRequestState::MustComplete,
              completionScopeForAction(op, event), "mpi_request_set_testsome");
        for (int index : event.request.completed_indices) {
          if (index < 0 ||
              static_cast<size_t>(index) >= event.request.requests.size()) {
            continue;
          }
          RequestID request =
              event.request.requests[static_cast<size_t>(index)];
          MPIRequestStateSummary &summary = ensureSummary(request, op, false);
          recordTransition(summary, MPIRequestActionKind::CompleteMust,
                           MPIRequestState::MustComplete, op.inst);
            summary.request_set_id = request_set_id;
            summary.completion_scope = completionScopeForAction(op, event);
            summary.provenance = "mpi_request_set_testsome";
        }
        }
        break;
      default:
        break;
      }
      break;
    case MPIRequestActionKind::None:
    case MPIRequestActionKind::CompleteMay:
      break;
    }
  }

  for (const auto &entry : request_state_summaries_) {
    const MPIRequestStateSummary &summary = entry.second;
    NonBlockingOp op;
    op.issue_inst =
        summary.activation_inst ? summary.activation_inst : summary.origin_inst;
    op.request = summary.request;
    op.completion_state = summary.state;
    op.wait_inst = summary.last_transition_inst;
    op.peer_rank = summary.peer_rank;
    op.tag = summary.tag;
    op.comm = summary.communicator;
    non_blocking_ops_[summary.request] = op;
  }

  for (MPIOperation &issued_op : all_operations_) {
    if (!issued_op.request) {
      continue;
    }
    auto it = request_state_summaries_.find(issued_op.request);
    if (it == request_state_summaries_.end()) {
      continue;
    }
    issued_op.request_state = it->second.state;
    issued_op.completion_inst = it->second.last_transition_inst;
  }
}

std::vector<MPIOperation>
MPIProcessModel::getOperationsByKind(MPIOpKind kind) const {
  std::vector<MPIOperation> result;
  for (const MPIOperation &op : all_operations_) {
    if (op.kind == kind) {
      result.push_back(op);
    }
  }
  return result;
}

MPICommunicationMatch
MPIProcessModel::classifyCommunicationMatch(const MPIOperation &op1,
                                            const MPIOperation &op2) const {
  auto subgroupsDefinitelyDisjoint = [](const MPIOperation &lhs,
                                        const MPIOperation &rhs) {
    if (lhs.communicator_subgroup_id == 0 || rhs.communicator_subgroup_id == 0) {
      return false;
    }
    if (lhs.communicator_subgroup_id == rhs.communicator_subgroup_id) {
      return false;
    }
    if (lhs.process_set_fact.subgroup_token_kind ==
            MPICommunicatorSubgroupTokenKind::None ||
        rhs.process_set_fact.subgroup_token_kind ==
            MPICommunicatorSubgroupTokenKind::None) {
      return false;
    }
    if (lhs.process_set_fact.subgroup_token_kind ==
            MPICommunicatorSubgroupTokenKind::SplitColorUnknown ||
        rhs.process_set_fact.subgroup_token_kind ==
            MPICommunicatorSubgroupTokenKind::SplitColorUnknown) {
      return false;
    }
    return true;
  };

  auto participantSetMayContainRank = [](const MPIParticipantSet &participants,
                                         int rank) {
    return participants.unknown || participants.contains(rank);
  };
  auto participantSetMayOverlapRange = [](const MPIParticipantSet &participants,
                                          int min_rank, int max_rank) {
    if (participants.unknown) {
      return true;
    }
    if (min_rank < 0 || max_rank < 0) {
      return true;
    }
    for (int rank = min_rank; rank <= max_rank; ++rank) {
      if (participants.contains(rank)) {
        return true;
      }
    }
    return false;
  };

  bool op1_is_send = (op1.kind == MPIOpKind::SEND_BLOCKING ||
                      op1.kind == MPIOpKind::SEND_NONBLOCKING);
  bool op2_is_send = (op2.kind == MPIOpKind::SEND_BLOCKING ||
                      op2.kind == MPIOpKind::SEND_NONBLOCKING);

  if (op1_is_send == op2_is_send) {
    return MPICommunicationMatch::NoMatch;
  }

  const MPIOperation &send = op1_is_send ? op1 : op2;
  const MPIOperation &recv = op1_is_send ? op2 : op1;
  bool precise = true;
  bool model_gap = false;

  if (!send.communicator && !recv.communicator) {
    model_gap = true;
    precise = false;
  }

  if (send.participant_set.unknown || recv.participant_set.unknown) {
    model_gap = true;
    precise = false;
  }

  if (!send.participant_set.unknown && !recv.participant_set.unknown &&
      send.participant_set.constrainsParticipants() &&
      recv.participant_set.constrainsParticipants()) {
    if (!isMPIWildcardValue(send.dest_rank)) {
      if (!participantSetMayContainRank(recv.participant_set, send.dest_rank)) {
        return MPICommunicationMatch::NoMatch;
      }
    } else if (!participantSetMayOverlapRange(recv.participant_set,
                                              send.dest_rank_min,
                                              send.dest_rank_max)) {
      return MPICommunicationMatch::NoMatch;
    }
    if (isMPIWildcardValue(send.dest_rank) || send.dest_rank < 0) {
      precise = false;
    }

    if (!isMPIWildcardValue(recv.source_rank)) {
      if (!participantSetMayContainRank(send.participant_set,
                                        recv.source_rank)) {
        return MPICommunicationMatch::NoMatch;
      }
    } else if (!participantSetMayOverlapRange(send.participant_set,
                                              recv.source_rank_min,
                                              recv.source_rank_max)) {
      return MPICommunicationMatch::NoMatch;
    }
    if (isMPIWildcardValue(recv.source_rank) || recv.source_rank < 0) {
      precise = false;
    }
  } else {
    if (!isMPIWildcardValue(send.dest_rank) &&
        !isMPIWildcardValue(recv.source_rank) &&
        send.dest_rank != recv.source_rank) {
      // In SPMD MPI, one function body is executed by many ranks. Same-function
      // send/recv pairs are therefore still viable unless rank/participant
      // facts prove them incompatible.
      precise = false;
    } else if (!rangesOverlap(send.dest_rank_min, send.dest_rank_max,
                              recv.source_rank_min, recv.source_rank_max)) {
      return MPICommunicationMatch::NoMatch;
    } else if (isMPIWildcardValue(send.dest_rank) ||
               isMPIWildcardValue(recv.source_rank) || send.dest_rank < 0 ||
               recv.source_rank < 0) {
      precise = false;
    }
  }

  if (!isMPIWildcardValue(send.tag) && !isMPIWildcardValue(recv.tag) &&
      send.tag != recv.tag) {
    return MPICommunicationMatch::NoMatch;
  }
  if (isMPIWildcardValue(send.tag) || isMPIWildcardValue(recv.tag) ||
      send.tag < 0 || recv.tag < 0) {
    precise = false;
  }

  if (send.communicator_class_id != 0 && recv.communicator_class_id != 0 &&
      send.communicator_class_id == recv.communicator_class_id) {
    if (subgroupsDefinitelyDisjoint(send, recv)) {
      return MPICommunicationMatch::NoMatch;
    }
  } else if (send.communicator && recv.communicator) {
    const CommunicatorAliasResult communicator_alias =
        classifyCommunicatorAlias(send.communicator, recv.communicator,
                                  &module_);
    if (communicator_alias == CommunicatorAliasResult::NoAlias) {
      return MPICommunicationMatch::NoMatch;
    }
    if (communicator_alias == CommunicatorAliasResult::Unknown) {
      model_gap = true;
      precise = false;
    }
  } else {
    model_gap = true;
    precise = false;
  }

  if (send.protocol_reachability == ProtocolReachability::SomeRanks &&
      recv.protocol_reachability == ProtocolReachability::SomeRanks &&
      !send.participant_set.mustEqual(recv.participant_set)) {
    precise = false;
  }

  if (send.datatype_size > 0 && recv.datatype_size > 0 &&
      send.datatype_size != recv.datatype_size) {
    return MPICommunicationMatch::NoMatch;
  }

  if (model_gap) {
    return precise ? MPICommunicationMatch::MayMatch
                   : MPICommunicationMatch::Unknown;
  }
  return precise ? MPICommunicationMatch::MustMatch
                 : MPICommunicationMatch::MayMatch;
}

bool MPIProcessModel::canCommunicate(const MPIOperation &op1,
                                     const MPIOperation &op2) const {
  MPICommunicationMatch match = classifyCommunicationMatch(op1, op2);
  return match == MPICommunicationMatch::MustMatch ||
         match == MPICommunicationMatch::MayMatch ||
         match == MPICommunicationMatch::Unknown;
}

std::vector<MPIProcessModel::NonBlockingOp>
MPIProcessModel::findOrphanedNonBlockingOps() const {
  std::vector<NonBlockingOp> orphaned;
  for (const auto &pair : non_blocking_ops_) {
    if (pair.second.completion_state == MPIRequestState::Pending ||
        pair.second.completion_state == MPIRequestState::Active ||
        pair.second.completion_state == MPIRequestState::Created) {
      orphaned.push_back(pair.second);
    }
  }
  return orphaned;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> deadlocks;
  struct DeadlockNode {
    const Instruction *inst = nullptr;
    const Function *function = nullptr;
    std::vector<RequestID> requests;
    std::vector<const MPIOperation *> origin_ops;
    bool send_side = false;
    bool recv_side = false;
    bool blocking = false;
    bool wait_side = false;
  };

  std::unordered_map<const Instruction *, const MPIEvent *> event_by_inst;
  for (const MPIEvent &event : semantic_events_) {
    event_by_inst[event.inst] = &event;
  }

  std::unordered_map<RequestID, const MPIOperation *> request_origin;
  for (const MPIOperation &op : all_operations_) {
    if (op.request) {
      request_origin.emplace(op.request, &op);
    }
  }

  std::vector<DeadlockNode> nodes;
  std::unordered_map<const Instruction *, size_t> node_by_inst;
  std::unordered_map<RequestID, size_t> wait_node_by_request;
  std::unordered_map<const Function *, const Instruction *> first_blocking_inst_by_function;
  std::unordered_map<const Function *, const MPIFunctionSummary *> summary_by_function;
  for (const MPIFunctionSummary &summary : function_summaries_) {
    summary_by_function[summary.function] = &summary;
  }
  std::unordered_map<size_t, const MPIChannelEndpointObligation *> endpoint_by_id;
  std::unordered_map<size_t, std::vector<const MPIChannelEndpointObligation *>>
      blocking_endpoints_by_operation;
  for (const MPIChannelEndpointObligation &endpoint : channel_endpoint_obligations_) {
    endpoint_by_id[endpoint.obligation_id] = &endpoint;
    if (endpoint.blocking) {
      blocking_endpoints_by_operation[endpoint.operation_index].push_back(&endpoint);
    }
  }
  std::map<std::pair<size_t, size_t>, const MPIChannelObligation *> obligation_by_pair;
  std::unordered_map<size_t, std::vector<const MPIChannelObligation *>>
      obligations_by_request_set;
  for (const MPIChannelObligation &obligation : channel_obligations_) {
    obligation_by_pair[std::make_pair(obligation.sender_obligation_id,
                                      obligation.receiver_obligation_id)] = &obligation;
    if (obligation.request_set_id != 0) {
      obligations_by_request_set[obligation.request_set_id].push_back(&obligation);
    }
  }

  auto instructionOrder = [&](const Instruction *inst) {
    auto op_it = std::find_if(all_operations_.begin(), all_operations_.end(),
                              [&](const MPIOperation &op) { return op.inst == inst; });
    return static_cast<size_t>(op_it - all_operations_.begin());
  };

  auto addNode = [&](const DeadlockNode &node) {
    auto it = node_by_inst.find(node.inst);
    if (it != node_by_inst.end()) {
      return it->second;
    }
    size_t node_id = nodes.size();
    nodes.push_back(node);
    node_by_inst[node.inst] = node_id;
    return node_id;
  };

  for (const auto &entry : summary_by_function) {
    const Function *function = entry.first;
    const MPIFunctionSummary &summary = *entry.second;
    for (size_t obligation_id : summary.blocking_endpoint_obligation_ids) {
      auto endpoint_it = endpoint_by_id.find(obligation_id);
      if (endpoint_it == endpoint_by_id.end()) {
        continue;
      }
      const Instruction *inst =
          all_operations_[endpoint_it->second->operation_index].inst;
      if (!inst) {
        continue;
      }
      auto first_it = first_blocking_inst_by_function.find(function);
      if (first_it == first_blocking_inst_by_function.end() ||
          instructionOrder(inst) < instructionOrder(first_it->second)) {
        first_blocking_inst_by_function[function] = inst;
      }
    }
  }

  for (const MPIOperation &op : all_operations_) {
    if (!op.function || !isBlockingPointToPointKind(op.kind)) {
      continue;
    }
    DeadlockNode node;
    node.inst = op.inst;
    node.function = op.function;
    node.origin_ops.push_back(&op);
    node.send_side = op.kind == MPIOpKind::SEND_BLOCKING;
    node.recv_side = op.kind == MPIOpKind::RECV_BLOCKING;
    node.blocking = true;
    addNode(node);
    auto first_it = first_blocking_inst_by_function.find(op.function);
    if (first_it == first_blocking_inst_by_function.end()) {
      first_blocking_inst_by_function[op.function] = op.inst;
    }
  }

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::WAIT || !op.function) {
      continue;
    }
    auto event_it = event_by_inst.find(op.inst);
    if (event_it == event_by_inst.end() || !event_it->second->has_request_semantics) {
      continue;
    }
    DeadlockNode node;
    node.inst = op.inst;
    node.function = op.function;
    node.blocking = true;
    node.wait_side = true;
    bool has_active_request = false;
    for (RequestID request : event_it->second->request.requests) {
      auto summary_it = request_state_summaries_.find(request);
      if (summary_it == request_state_summaries_.end()) {
        continue;
      }
      if (isResolvedRequestState(summary_it->second.state) &&
          summary_it->second.last_transition_inst != op.inst) {
        continue;
      }
      auto origin_it = request_origin.find(request);
      if (origin_it == request_origin.end() || !origin_it->second) {
        continue;
      }
      has_active_request = true;
      node.requests.push_back(request);
      node.origin_ops.push_back(origin_it->second);
      node.send_side = node.send_side || isSendOperationKind(origin_it->second->kind);
      node.recv_side = node.recv_side || isRecvOperationKind(origin_it->second->kind);
    }
    if (!has_active_request) {
      continue;
    }
    size_t node_id = addNode(node);
    for (RequestID request : node.requests) {
      wait_node_by_request.emplace(request, node_id);
    }
    auto first_it = first_blocking_inst_by_function.find(op.function);
    if (first_it == first_blocking_inst_by_function.end()) {
      first_blocking_inst_by_function[op.function] = op.inst;
    }
  }

  struct DeadlockEdge {
    size_t target = 0;
    MPICommunicationMatch proof = MPICommunicationMatch::Unknown;
  };
  std::vector<std::vector<DeadlockEdge>> graph(nodes.size());
  auto addEdge = [&](size_t source, size_t target, MPICommunicationMatch proof) {
    if (source >= graph.size() || target >= graph.size() || source == target ||
        nodes[source].function == nodes[target].function) {
      return;
    }
    for (const DeadlockEdge &edge : graph[source]) {
      if (edge.target == target && edge.proof == proof) {
        return;
      }
    }
    graph[source].push_back({target, proof});
  };

  auto addDependencyFromChannel = [&](size_t source_node,
                                      const MPIOperation &counterpart,
                                      RequestID counterpart_request,
                                      MPICommunicationMatch proof) {
    auto counterpart_node = node_by_inst.find(counterpart.inst);
    if (counterpart_node != node_by_inst.end()) {
      addEdge(source_node, counterpart_node->second, proof);
      return;
    }
    auto wait_node_it = wait_node_by_request.find(counterpart_request);
    if (counterpart_request && wait_node_it != wait_node_by_request.end()) {
      addEdge(source_node, wait_node_it->second, proof);
    }
  };

  auto opMayExecuteOnRank = [](const MPIOperation &op, int rank) {
    if (rank < 0) {
      return true;
    }
    if (op.process_rank.kind == MPI::RankExpr::Concrete) {
      return op.process_rank.concrete_value == rank;
    }
    if (op.process_rank.kind == MPI::RankExpr::Range) {
      return rank >= op.process_rank.range_min && rank <= op.process_rank.range_max;
    }
    return true;
  };

  auto blockingCompatibility = [&](const MPIOperation &send,
                                   const MPIOperation &recv) {
    if (!isSendOperationKind(send.kind) || !isRecvOperationKind(recv.kind)) {
      return MPICommunicationMatch::NoMatch;
    }
    if (!sameCommunicatorForProof(send, recv, &module_)) {
      return MPICommunicationMatch::NoMatch;
    }
    if (!isMPIWildcardValue(send.dest_rank) &&
        !opMayExecuteOnRank(recv, send.dest_rank)) {
      return MPICommunicationMatch::NoMatch;
    }
    if (!isMPIWildcardValue(recv.source_rank) &&
        !opMayExecuteOnRank(send, recv.source_rank)) {
      return MPICommunicationMatch::NoMatch;
    }
    if (!isMPIWildcardValue(send.tag) && !isMPIWildcardValue(recv.tag) &&
        send.tag != recv.tag) {
      return MPICommunicationMatch::NoMatch;
    }
    if (send.participant_set.unknown || recv.participant_set.unknown ||
        isMPIWildcardValue(send.tag) || isMPIWildcardValue(recv.tag)) {
      return MPICommunicationMatch::MayMatch;
    }
    return MPICommunicationMatch::MustMatch;
  };

  for (size_t node_id = 0; node_id < nodes.size(); ++node_id) {
    const DeadlockNode &node = nodes[node_id];
    if (!node.wait_side) {
      const MPIOperation *op = node.origin_ops.empty() ? nullptr : node.origin_ops.front();
      if (!op) {
        continue;
      }
      for (const MPIOperation &counterpart : all_operations_) {
        if (!counterpart.function || counterpart.function == op->function) {
          continue;
        }
        if (op->kind == MPIOpKind::SEND_BLOCKING &&
            counterpart.kind == MPIOpKind::RECV_BLOCKING) {
          MPICommunicationMatch proof = blockingCompatibility(*op, counterpart);
          if (proof != MPICommunicationMatch::NoMatch) {
            addDependencyFromChannel(node_id, counterpart, counterpart.request, proof);
          }
        } else if (op->kind == MPIOpKind::RECV_BLOCKING &&
                   counterpart.kind == MPIOpKind::SEND_BLOCKING) {
          MPICommunicationMatch proof = blockingCompatibility(counterpart, *op);
          if (proof != MPICommunicationMatch::NoMatch) {
            addDependencyFromChannel(node_id, counterpart, counterpart.request, proof);
          }
        }
      }
      continue;
    }

    for (const MPIOperation *origin_op : node.origin_ops) {
      if (!origin_op) {
        continue;
      }
      for (const MPIOperation &counterpart : all_operations_) {
        if (!counterpart.function || counterpart.function == origin_op->function) {
          continue;
        }
        if (isSendOperationKind(origin_op->kind) && isRecvOperationKind(counterpart.kind)) {
          MPICommunicationMatch proof = blockingCompatibility(*origin_op, counterpart);
          if (proof != MPICommunicationMatch::NoMatch) {
            addDependencyFromChannel(node_id, counterpart, counterpart.request, proof);
          }
        } else if (isRecvOperationKind(origin_op->kind) &&
                   isSendOperationKind(counterpart.kind)) {
          MPICommunicationMatch proof = blockingCompatibility(counterpart, *origin_op);
          if (proof != MPICommunicationMatch::NoMatch) {
            addDependencyFromChannel(node_id, counterpart, counterpart.request, proof);
          }
        }
      }
    }
  }

  std::set<std::pair<const Instruction *, const Instruction *>> unique_deadlocks;

  auto shouldReportCycle = [&](const std::vector<size_t> &cycle) {
    if (cycle.size() < 2) {
      return false;
    }
    std::set<size_t> cycle_nodes(cycle.begin(), cycle.end());
    bool has_must_internal = false;
    bool all_internal_nonmust = true;
    bool has_nonunknown_exit = false;
    bool has_any_internal = false;
    for (size_t node_id : cycle) {
      for (const DeadlockEdge &edge : graph[node_id]) {
        if (cycle_nodes.count(edge.target)) {
          has_any_internal = true;
          if (edge.proof == MPICommunicationMatch::MustMatch) {
            has_must_internal = true;
            all_internal_nonmust = false;
          } else if (edge.proof == MPICommunicationMatch::Unknown) {
            all_internal_nonmust = false;
          }
        } else if (edge.proof == MPICommunicationMatch::MustMatch ||
                   edge.proof == MPICommunicationMatch::MayMatch) {
          has_nonunknown_exit = true;
        }
      }
    }
    if (!has_any_internal) {
      return false;
    }
    if (has_must_internal) {
      return true;
    }
    for (size_t node_id : cycle) {
      auto summary_it = summary_by_function.find(nodes[node_id].function);
      if (summary_it != summary_by_function.end() &&
          summary_it->second->unresolved_indirect_call_effect) {
        return true;
      }
    }
    if (all_internal_nonmust && has_nonunknown_exit) {
      return false;
    }
    return true;
  };

  std::vector<int> color(nodes.size(), 0);
  std::vector<size_t> stack;
  std::function<void(size_t)> dfs = [&](size_t node_id) {
    color[node_id] = 1;
    stack.push_back(node_id);
    for (const DeadlockEdge &edge : graph[node_id]) {
      if (color[edge.target] == 0) {
        dfs(edge.target);
        continue;
      }
      if (color[edge.target] != 1) {
        continue;
      }
      auto begin = std::find(stack.begin(), stack.end(), edge.target);
      if (begin == stack.end()) {
        continue;
      }
      std::vector<size_t> cycle(begin, stack.end());
      if (!shouldReportCycle(cycle)) {
        continue;
      }
      std::vector<const Instruction *> members;
      std::set<const Function *> cycle_functions;
      for (size_t member : cycle) {
        const DeadlockNode &cycle_node = nodes[member];
        if (cycle_node.function) {
          cycle_functions.insert(cycle_node.function);
        }
      }
      for (const Function *function : cycle_functions) {
        auto first_it = first_blocking_inst_by_function.find(function);
        if (first_it != first_blocking_inst_by_function.end()) {
          members.push_back(first_it->second);
        }
      }
      if (members.size() < 2) {
        members.clear();
        for (size_t member : cycle) {
          if (nodes[member].inst) {
            members.push_back(nodes[member].inst);
          }
        }
      }
      std::sort(members.begin(), members.end());
      members.erase(std::unique(members.begin(), members.end()), members.end());
      if (members.size() < 2) {
        continue;
      }
      for (size_t i = 0; i < members.size(); ++i) {
        const Instruction *lhs = members[i];
        const Instruction *rhs = members[(i + 1) % members.size()];
        unique_deadlocks.emplace(lhs < rhs ? lhs : rhs, lhs < rhs ? rhs : lhs);
      }
    }
    stack.pop_back();
    color[node_id] = 2;
  };

  for (size_t node_id = 0; node_id < nodes.size(); ++node_id) {
    if (color[node_id] == 0) {
      dfs(node_id);
    }
  }

  deadlocks.assign(unique_deadlocks.begin(), unique_deadlocks.end());
  return deadlocks;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findTagMismatches() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> mismatches;
  std::set<std::pair<const Instruction *, const Instruction *>> added;
  for (const MPIChannelObligation &channel : channel_obligations_) {
    const MPIOperation &send = all_operations_[channel.sender_operation_index];
    const MPIOperation &recv = all_operations_[channel.receiver_operation_index];
    if (send.tag < 0 || recv.tag < 0 || send.tag == recv.tag) {
      continue;
    }
    std::pair<const Instruction *, const Instruction *> pair(send.inst, recv.inst);
    if (added.insert(pair).second) {
      mismatches.push_back(pair);
    }
  }

  return mismatches;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findCountDatatypeMismatches() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> mismatches;

  std::set<std::pair<const Instruction *, const Instruction *>> added;

  for (const MPIChannelObligation &channel : channel_obligations_) {
    const MPIOperation &send = all_operations_[channel.sender_operation_index];
    const MPIOperation &recv = all_operations_[channel.receiver_operation_index];
    bool count_mismatch = send.byte_length > 0 && recv.byte_length > 0 &&
                          send.byte_length != recv.byte_length;
    bool datatype_mismatch =
        send.datatype && recv.datatype && send.datatype != recv.datatype &&
        send.datatype_size > 0 && recv.datatype_size > 0 &&
        send.datatype_size != recv.datatype_size;

    if (count_mismatch || datatype_mismatch) {
      auto pair = std::make_pair(send.inst, recv.inst);
      if (added.insert(pair).second) {
        mismatches.push_back(pair);
      }
    }
  }

  return mismatches;
}

std::vector<const Instruction *> MPIProcessModel::findRankOutOfBounds() const {
  std::vector<const Instruction *> out_of_bounds;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::SEND_BLOCKING &&
        op.kind != MPIOpKind::SEND_NONBLOCKING &&
        op.kind != MPIOpKind::RECV_BLOCKING &&
        op.kind != MPIOpKind::RECV_NONBLOCKING) {
      continue;
    }

    int communicator_max_rank = -1;
    getCommunicatorRankUpperBound(op, rank_analysis_.get(), communicator_max_rank);

    if (op.kind == MPIOpKind::SEND_BLOCKING ||
        op.kind == MPIOpKind::SEND_NONBLOCKING) {
      if (rankValueDefinitelyOutOfBounds(op.dest_rank, communicator_max_rank,
                                         false) ||
          rankRangeDefinitelyOutOfBounds(op.dest_rank_min, op.dest_rank_max,
                                         communicator_max_rank)) {
        out_of_bounds.push_back(op.inst);
        continue;
      }
    }

    if (op.kind == MPIOpKind::RECV_BLOCKING ||
        op.kind == MPIOpKind::RECV_NONBLOCKING) {
      if (rankValueDefinitelyOutOfBounds(op.source_rank, communicator_max_rank,
                                         true) ||
          rankRangeDefinitelyOutOfBounds(op.source_rank_min, op.source_rank_max,
                                         communicator_max_rank)) {
        out_of_bounds.push_back(op.inst);
      }
    }
  }

  return out_of_bounds;
}

std::vector<RequestID> MPIProcessModel::findPersistentRequestLeaks() const {
  std::vector<RequestID> leaks;

  for (const auto &pair : request_state_summaries_) {
    const MPIRequestStateSummary &summary = pair.second;
    if (!summary.is_persistent) {
      continue;
    }
    if (summary.state == MPIRequestState::Pending ||
        summary.state == MPIRequestState::Created ||
        summary.state == MPIRequestState::Active) {
      leaks.push_back(pair.first);
    }
  }

  return leaks;
}

std::vector<const Instruction *>
MPIProcessModel::findCancelWithoutWait() const {
  std::vector<const Instruction *> issues;

  std::map<RequestID, const Instruction *> cancel_ops;
  std::set<RequestID> observed_after_cancel;

  for (const MPIOperation &op : all_operations_) {
    if (op.td_type == ThreadAPI::TD_MPI_CANCEL && op.request) {
      cancel_ops[op.request] = op.inst;
      continue;
    }

    if (!op.request) {
      continue;
    }

    if ((op.kind == MPIOpKind::WAIT || op.kind == MPIOpKind::TEST) &&
        cancel_ops.count(op.request)) {
      observed_after_cancel.insert(op.request);
    }
  }

  for (const auto &entry : cancel_ops) {
    if (!observed_after_cancel.count(entry.first)) {
      issues.push_back(entry.second);
    }
  }

  return issues;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findBufferOverlaps() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> overlaps;

  for (const MPIOperation &op : all_operations_) {
    if (op.td_type != ThreadAPI::TD_MPI_SENDRECV) {
      continue;
    }

    const CallBase *cb = dyn_cast<CallBase>(op.inst);
    if (!cb || cb->arg_size() < 11) {
      continue;
    }

    const Value *sendbuf = cb->getArgOperand(0);
    const Value *recvbuf = cb->getArgOperand(5);

    if (sendbuf && recvbuf && sendbuf == recvbuf) {
      overlaps.emplace_back(op.inst, op.inst);
    }
  }

  return overlaps;
}

std::vector<const Instruction *>
MPIProcessModel::findWildcardInCollective() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.source_rank == -1) {
      issues.push_back(op.inst);
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInPlaceConflicts() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.td_type == ThreadAPI::TD_MPI_GATHER ||
        op.td_type == ThreadAPI::TD_MPI_ALLGATHER ||
        op.td_type == ThreadAPI::TD_MPI_ALLTOALL) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *sendbuf = cb->getArgOperand(0);
        if (isLikelyMPIInPlace(sendbuf)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findNullHandles() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind == MPIOpKind::REQUEST_MANAGEMENT ||
        op.kind == MPIOpKind::COMM_MANAGEMENT) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *arg = cb->getArgOperand(0);
        if (isLikelyNullHandle(arg)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findNegativeRoot() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.td_type == ThreadAPI::TD_MPI_BCAST ||
        op.td_type == ThreadAPI::TD_MPI_REDUCE ||
        op.td_type == ThreadAPI::TD_MPI_GATHER ||
        op.td_type == ThreadAPI::TD_MPI_SCATTER) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb) {
        int root_arg = -1;
        switch (op.td_type) {
        case ThreadAPI::TD_MPI_BCAST:
          root_arg = 3;
          break;
        case ThreadAPI::TD_MPI_REDUCE:
          root_arg = 5;
          break;
        case ThreadAPI::TD_MPI_GATHER:
        case ThreadAPI::TD_MPI_SCATTER:
          root_arg = 6;
          break;
        default:
          break;
        }
        if (root_arg >= 0 && static_cast<unsigned>(root_arg) < cb->arg_size()) {
          if (const auto *ci =
                  dyn_cast<ConstantInt>(cb->getArgOperand(root_arg))) {
            int64_t root = ci->getSExtValue();
            if (root < 0) {
              issues.push_back(op.inst);
              continue;
            }
            int communicator_max_rank = -1;
            if (getCommunicatorRankUpperBound(op, rank_analysis_.get(),
                                              communicator_max_rank) &&
                root > communicator_max_rank) {
              issues.push_back(op.inst);
            }
          }
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInvalidTags() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::SEND_BLOCKING &&
        op.kind != MPIOpKind::SEND_NONBLOCKING &&
        op.kind != MPIOpKind::RECV_BLOCKING &&
        op.kind != MPIOpKind::RECV_NONBLOCKING) {
      continue;
    }

    if ((op.kind == MPIOpKind::SEND_BLOCKING ||
         op.kind == MPIOpKind::SEND_NONBLOCKING)) {
      if (op.tag < 0) {
        issues.push_back(op.inst);
      }
      continue;
    }

    if (op.tag < -1) {
      issues.push_back(op.inst);
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInvalidRanks() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::SEND_BLOCKING &&
        op.kind != MPIOpKind::SEND_NONBLOCKING &&
        op.kind != MPIOpKind::RECV_BLOCKING &&
        op.kind != MPIOpKind::RECV_NONBLOCKING) {
      continue;
    }

    int communicator_max_rank = -1;
    getCommunicatorRankUpperBound(op, rank_analysis_.get(), communicator_max_rank);

    if (op.kind == MPIOpKind::SEND_BLOCKING ||
        op.kind == MPIOpKind::SEND_NONBLOCKING) {
      if (rankValueDefinitelyOutOfBounds(op.dest_rank, communicator_max_rank,
                                         false) ||
          rankRangeDefinitelyOutOfBounds(op.dest_rank_min, op.dest_rank_max,
                                         communicator_max_rank)) {
        issues.push_back(op.inst);
        continue;
      }
    }

    if (op.kind == MPIOpKind::RECV_BLOCKING ||
        op.kind == MPIOpKind::RECV_NONBLOCKING) {
      if (rankValueDefinitelyOutOfBounds(op.source_rank, communicator_max_rank,
                                         true) ||
          rankRangeDefinitelyOutOfBounds(op.source_rank_min, op.source_rank_max,
                                         communicator_max_rank)) {
        issues.push_back(op.inst);
      }
    }
  }

  return issues;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
MPIProcessModel::findTypeSizeMismatches() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> mismatches;
  std::set<std::pair<const Instruction *, const Instruction *>> added;

  for (const MPIOperation &send_op : all_operations_) {
    if (send_op.kind != MPIOpKind::SEND_BLOCKING &&
        send_op.kind != MPIOpKind::SEND_NONBLOCKING) {
      continue;
    }

    for (const MPIOperation &recv_op : all_operations_) {
      if (recv_op.kind != MPIOpKind::RECV_BLOCKING &&
          recv_op.kind != MPIOpKind::RECV_NONBLOCKING) {
        continue;
      }
      if (!sameCommunicatorForProof(send_op, recv_op, &module_)) {
        continue;
      }
      if (!rangesOverlap(send_op.dest_rank_min, send_op.dest_rank_max,
                         recv_op.source_rank_min, recv_op.source_rank_max)) {
        continue;
      }
      if (send_op.byte_length <= 0 || recv_op.byte_length <= 0) {
        continue;
      }
      if (send_op.byte_length == recv_op.byte_length) {
        continue;
      }
      auto pair = std::make_pair(send_op.inst, recv_op.inst);
      if (added.insert(pair).second) {
        mismatches.push_back(pair);
      }
    }
  }

  return mismatches;
}

std::vector<const Instruction *> MPIProcessModel::findDestroyNullComm() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COMM_MANAGEMENT) {
      continue;
    }
    if (op.td_type == ThreadAPI::TD_MPI_COMM_FREE) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *comm = cb->getArgOperand(0);
        if (isLikelyNullHandle(comm)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

std::vector<const Instruction *>
MPIProcessModel::findRequestFreeAfterWait() const {
  std::vector<const Instruction *> issues;
  for (const auto &entry : request_state_summaries_) {
    const MPIRequestStateSummary &summary = entry.second;
    const Instruction *free_inst = nullptr;
    bool observed_completion_before_free = false;
    for (const MPIRequestTransition &transition : summary.history) {
      if (transition.action == MPIRequestActionKind::Free) {
        free_inst = transition.inst;
        break;
      }
      if (transition.action == MPIRequestActionKind::CompleteMust) {
        observed_completion_before_free = true;
      }
    }
    if (observed_completion_before_free && free_inst) {
      issues.push_back(free_inst);
    }
  }

  return issues;
}

std::vector<const Instruction *> MPIProcessModel::findInPlaceWrongOp() const {
  std::vector<const Instruction *> issues;

  for (const MPIOperation &op : all_operations_) {
    if (op.kind != MPIOpKind::COLLECTIVE_BLOCKING &&
        op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }

    if (op.td_type == ThreadAPI::TD_MPI_REDUCE ||
        op.td_type == ThreadAPI::TD_MPI_SCAN ||
        op.td_type == ThreadAPI::TD_MPI_BCAST ||
        op.td_type == ThreadAPI::TD_MPI_SCATTER) {
      const CallBase *cb = dyn_cast<CallBase>(op.inst);
      if (cb && cb->arg_size() > 0) {
        const Value *sendbuf = cb->getArgOperand(0);
        if (isLikelyMPIInPlace(sendbuf)) {
          issues.push_back(op.inst);
        }
      }
    }
  }

  return issues;
}

} // namespace mpi

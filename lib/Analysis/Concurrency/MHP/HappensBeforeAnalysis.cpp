#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/OpenMP/OpenMPSemantics.h"
#include "Analysis/Concurrency/Utils/ThreadMultiplicity.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

namespace {

bool hasBranchWitness(const Instruction *inst) {
  if (!inst) {
    return false;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }
      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        for (const User *cmp_user : cmp->users()) {
          const auto *branch = dyn_cast<BranchInst>(cmp_user);
          if (branch && branch->isConditional()) {
            return true;
          }
        }
        continue;
      }
      if (isa<CastInst>(user) || isa<BinaryOperator>(user) ||
          isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return false;
}

bool hasCmpXchgSuccessWitness(const Instruction *inst) {
  if (!isa<AtomicCmpXchgInst>(inst)) {
    return false;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }
      if (const auto *extract = dyn_cast<ExtractValueInst>(user)) {
        if (extract->getNumIndices() == 1 && extract->getIndices()[0] == 1) {
          worklist.push_back(extract);
        }
        continue;
      }
      if (const auto *branch = dyn_cast<BranchInst>(user)) {
        if (branch->isConditional() && branch->getCondition() == current) {
          return true;
        }
        continue;
      }
      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        for (const User *cmp_user : cmp->users()) {
          const auto *branch = dyn_cast<BranchInst>(cmp_user);
          if (branch && branch->isConditional()) {
            return true;
          }
        }
        continue;
      }
      if (isa<CastInst>(user) || isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return false;
}

const BranchInst *getBranchWitness(const Instruction *inst) {
  if (!inst) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }
      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        for (const User *cmp_user : cmp->users()) {
          const auto *branch = dyn_cast<BranchInst>(cmp_user);
          if (branch && branch->isConditional()) {
            return branch;
          }
        }
        continue;
      }
      if (isa<CastInst>(user) || isa<BinaryOperator>(user) ||
          isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return nullptr;
}

const ICmpInst *getAssumeWitnessCmp(const Instruction *inst) {
  if (!inst) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(inst);
  visited.insert(inst);

  const ICmpInst *assume_cmp = nullptr;
  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    for (const User *user : current->users()) {
      if (!visited.insert(user).second) {
        continue;
      }

      if (const auto *cmp = dyn_cast<ICmpInst>(user)) {
        bool feeds_assume = false;
        for (const User *cmp_user : cmp->users()) {
          const auto *call = dyn_cast<CallBase>(cmp_user);
          const auto *intrinsic = dyn_cast_or_null<IntrinsicInst>(call);
          if (!intrinsic || intrinsic->getIntrinsicID() != Intrinsic::assume) {
            continue;
          }
          if (call->arg_size() == 0 || call->getArgOperand(0) != cmp) {
            continue;
          }
          feeds_assume = true;
          if (!assume_cmp) {
            assume_cmp = cmp;
          } else if (assume_cmp != cmp) {
            return nullptr;
          }
        }
        if (feeds_assume) {
          continue;
        }
      }

      if (isa<CastInst>(user) || isa<BinaryOperator>(user) ||
          isa<SelectInst>(user) || isa<PHINode>(user)) {
        worklist.push_back(user);
      }
    }
  }

  return assume_cmp;
}

bool hasAssumeWitness(const Instruction *inst) {
  return getAssumeWitnessCmp(inst) != nullptr;
}

const BasicBlock *getWitnessSuccessor(const Instruction *inst) {
  const auto *branch = getBranchWitness(inst);
  if (!branch) {
    return nullptr;
  }

  const auto *cmp = dyn_cast<ICmpInst>(branch->getCondition());
  if (!cmp) {
    return nullptr;
  }

  if (cmp->getPredicate() != ICmpInst::ICMP_EQ &&
      cmp->getPredicate() != ICmpInst::ICMP_NE) {
    return nullptr;
  }

  const Value *lhs = cmp->getOperand(0);
  const Value *rhs = cmp->getOperand(1);
  const auto *lhs_const = dyn_cast<ConstantInt>(lhs);
  const auto *rhs_const = dyn_cast<ConstantInt>(rhs);
  const ConstantInt *constant = lhs_const ? lhs_const : rhs_const;
  if (!constant) {
    return nullptr;
  }

  auto choose = [&](bool sync_on_true) -> const BasicBlock * {
    return branch->getSuccessor(sync_on_true ? 0 : 1);
  };

  const bool is_zero = constant->isZero();
  if (cmp->getPredicate() == ICmpInst::ICMP_NE) {
    return choose(is_zero);
  }
  return choose(!is_zero);
}

struct AtomicSyncWitness {
  const BasicBlock *successor = nullptr;
  const ConstantInt *constant = nullptr;
  bool requires_nonzero = false;
  bool valid = false;
};

AtomicSyncWitness getAtomicSyncWitness(const Instruction *inst) {
  AtomicSyncWitness witness;
  const ICmpInst *cmp = nullptr;
  bool sync_on_true = true;

  if (const auto *branch = getBranchWitness(inst)) {
    cmp = dyn_cast<ICmpInst>(branch->getCondition());
    if (!cmp) {
      return witness;
    }

    const Value *lhs = cmp->getOperand(0);
    const Value *rhs = cmp->getOperand(1);
    const auto *lhs_const = dyn_cast<ConstantInt>(lhs);
    const auto *rhs_const = dyn_cast<ConstantInt>(rhs);
    const ConstantInt *constant = lhs_const ? lhs_const : rhs_const;
    if (!constant) {
      return witness;
    }

    const bool is_zero = constant->isZero();
    if (cmp->getPredicate() == ICmpInst::ICMP_NE) {
      sync_on_true = is_zero;
    } else {
      sync_on_true = !is_zero;
    }
    witness.successor = branch->getSuccessor(sync_on_true ? 0 : 1);
    if (!witness.successor) {
      return witness;
    }
  } else {
    cmp = getAssumeWitnessCmp(inst);
    if (!cmp) {
      return witness;
    }
    sync_on_true = true;
  }

  if (cmp->getPredicate() != ICmpInst::ICMP_EQ &&
      cmp->getPredicate() != ICmpInst::ICMP_NE) {
    return witness;
  }

  const Value *lhs = cmp->getOperand(0);
  const Value *rhs = cmp->getOperand(1);
  const auto *lhs_const = dyn_cast<ConstantInt>(lhs);
  const auto *rhs_const = dyn_cast<ConstantInt>(rhs);
  const ConstantInt *constant = lhs_const ? lhs_const : rhs_const;
  if (!constant) {
    return witness;
  }

  bool witnesses_nonzero = false;
  bool witnesses_exact_constant = false;
  if (constant->isZero()) {
    witnesses_nonzero =
        (cmp->getPredicate() == ICmpInst::ICMP_NE && sync_on_true) ||
        (cmp->getPredicate() == ICmpInst::ICMP_EQ && !sync_on_true);
  } else {
    witnesses_exact_constant =
        (cmp->getPredicate() == ICmpInst::ICMP_EQ && sync_on_true) ||
        (cmp->getPredicate() == ICmpInst::ICMP_NE && !sync_on_true);
  }

  if (!witnesses_nonzero && !witnesses_exact_constant) {
    return witness;
  }

  witness.constant = constant;
  witness.requires_nonzero = witnesses_nonzero;
  witness.valid = true;
  return witness;
}

const ConstantInt *getAtomicStoredConstant(const Instruction *inst) {
  if (!inst) {
    return nullptr;
  }
  if (const auto *store = dyn_cast<StoreInst>(inst)) {
    return dyn_cast<ConstantInt>(store->getValueOperand());
  }
  if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst)) {
    return dyn_cast<ConstantInt>(cmpxchg->getNewValOperand());
  }
  if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    if (rmw->getOperation() == AtomicRMWInst::Xchg) {
      return dyn_cast<ConstantInt>(rmw->getValOperand());
    }
  }
  return nullptr;
}

bool constantIntsEqual(const ConstantInt *lhs, const ConstantInt *rhs) {
  if (!lhs || !rhs) {
    return false;
  }
  const unsigned width =
      std::max(lhs->getValue().getBitWidth(), rhs->getValue().getBitWidth());
  return lhs->getValue().zextOrTrunc(width) ==
         rhs->getValue().zextOrTrunc(width);
}

const ConstantInt *getAtomicInitialConstant(const Instruction *inst) {
  if (!inst) {
    return nullptr;
  }

  const Value *ptr = CppAtomics::getAtomicPointer(inst);
  ptr = ptr ? ptr->stripPointerCasts() : nullptr;
  const Value *base = ptr ? getUnderlyingObject(ptr) : nullptr;
  const auto *global = dyn_cast_or_null<GlobalVariable>(
      base ? base->stripPointerCasts() : nullptr);
  if (!global || !global->hasInitializer()) {
    return nullptr;
  }
  return dyn_cast<ConstantInt>(global->getInitializer());
}

const ConstantInt *evaluateAtomicRMWValue(const AtomicRMWInst *rmw,
                                          const ConstantInt *current_value) {
  if (!rmw || !current_value) {
    return nullptr;
  }

  const auto *operand = dyn_cast<ConstantInt>(rmw->getValOperand());
  if (!operand) {
    return nullptr;
  }

  const unsigned width =
      std::max(current_value->getBitWidth(), operand->getBitWidth());
  APInt lhs = current_value->getValue().zextOrTrunc(width);
  APInt rhs = operand->getValue().zextOrTrunc(width);

  switch (rmw->getOperation()) {
  case AtomicRMWInst::Add:
    return ConstantInt::get(rmw->getContext(), lhs + rhs);
  case AtomicRMWInst::Sub:
    return ConstantInt::get(rmw->getContext(), lhs - rhs);
  case AtomicRMWInst::And:
    return ConstantInt::get(rmw->getContext(), lhs & rhs);
  case AtomicRMWInst::Or:
    return ConstantInt::get(rmw->getContext(), lhs | rhs);
  case AtomicRMWInst::Xor:
    return ConstantInt::get(rmw->getContext(), lhs ^ rhs);
  case AtomicRMWInst::Xchg:
    return ConstantInt::get(rmw->getContext(), rhs);
  default:
    return nullptr;
  }
}

bool candidateMatchesAcquireWitness(const ConstantInt *candidate,
                                    const Instruction *acquire_anchor,
                                    const ConstantInt *initial_value) {
  if (!candidate || !acquire_anchor) {
    return false;
  }

  if (!isa<LoadInst>(acquire_anchor)) {
    return true;
  }

  const AtomicSyncWitness witness = getAtomicSyncWitness(acquire_anchor);
  if (!witness.valid) {
    return false;
  }

  if (witness.requires_nonzero) {
    if (candidate->isZero()) {
      return false;
    }
    return initial_value && initial_value->isZero();
  }

  if (!constantIntsEqual(candidate, witness.constant)) {
    return false;
  }
  return !initial_value || !constantIntsEqual(initial_value, witness.constant);
}

bool releaseMatchesAcquireWitness(const Instruction *release_inst,
                                  const Instruction *acquire_anchor) {
  if (!release_inst || !acquire_anchor) {
    return false;
  }

  if (!isa<LoadInst>(acquire_anchor)) {
    return true;
  }

  const AtomicSyncWitness witness = getAtomicSyncWitness(acquire_anchor);
  if (!witness.valid) {
    return false;
  }

  const ConstantInt *release_value = getAtomicStoredConstant(release_inst);
  if (!release_value) {
    return false;
  }
  const ConstantInt *initial_value = getAtomicInitialConstant(release_inst);

  if (witness.requires_nonzero) {
    return !release_value->isZero() &&
           (!initial_value || initial_value->isZero());
  }
  return constantIntsEqual(release_value, witness.constant) &&
         (!initial_value ||
          !constantIntsEqual(initial_value, witness.constant));
}

struct AtomicLocationKey {
  const Value *base = nullptr;
  int64_t offset = 0;
  bool has_precise_offset = false;

  bool operator==(const AtomicLocationKey &other) const {
    return base == other.base && offset == other.offset &&
           has_precise_offset == other.has_precise_offset;
  }
};

struct AtomicLocationKeyHash {
  size_t operator()(const AtomicLocationKey &key) const {
    size_t seed = std::hash<const Value *>{}(key.base);
    seed ^= std::hash<int64_t>{}(key.offset) + 0x9e3779b9 + (seed << 6) +
            (seed >> 2);
    seed ^= std::hash<bool>{}(key.has_precise_offset) + 0x9e3779b9 +
            (seed << 6) + (seed >> 2);
    return seed;
  }
};

AtomicLocationKey getExactAtomicLocation(const Instruction *inst) {
  AtomicLocationKey key;
  const Value *ptr = CppAtomics::getAtomicPointer(inst);
  if (!ptr) {
    return key;
  }

  ptr = ptr->stripPointerCasts();
  int64_t offset = 0;
  if (const Value *base = GetPointerBaseWithConstantOffset(
          ptr, offset, inst->getModule()->getDataLayout())) {
    key.base = base->stripPointerCasts();
    key.offset = offset;
    key.has_precise_offset = true;
    return key;
  }

  if (const Value *base = getUnderlyingObject(ptr)) {
    key.base = base->stripPointerCasts();
    return key;
  }
  key.base = ptr;
  return key;
}

} // namespace

HappensBeforeAnalysis::HappensBeforeAnalysis(Module &module,
                                             mhp::MHPAnalysis &mhp)
    : m_module(module), m_mhp(mhp), m_alias_analysis(mhp.getAliasAnalysis()) {}

void HappensBeforeAnalysis::analyze() {
  m_hb_cache.clear();
  m_future_shared_state.clear();
  m_shared_state_trace_cache.clear();
  m_deferred_sync_counts.clear();
  m_atomic_instructions.clear();
  m_sync_with.clear();
  m_explicit_hb_pairs.clear();
  m_extra_hb_successors.clear();
  m_post_dom_cache.clear();

  computeAtomicHappensBefore();
  buildSynchronizesWith();
}

void HappensBeforeAnalysis::addExtraHBEdge(const Instruction *from,
                                           const Instruction *to) {
  if (!from || !to || from == to) {
    return;
  }

  mhp::ThreadID from_tid = m_mhp.getThreadID(from);
  mhp::ThreadID to_tid = m_mhp.getThreadID(to);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> from_nodes =
      from_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(from)
          : tfg.getNodes(from, from_tid);
  std::vector<mhp::SyncNode *> to_nodes =
      to_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(to)
          : tfg.getNodes(to, to_tid);
  for (mhp::SyncNode *from_node : from_nodes) {
    auto &succs = m_extra_hb_successors[from_node];
    for (mhp::SyncNode *to_node : to_nodes) {
      if (std::find(succs.begin(), succs.end(), to_node) == succs.end()) {
        succs.push_back(to_node);
      }
    }
  }
}

void HappensBeforeAnalysis::addExtraHBEdge(const mhp::SyncNode *from,
                                           const mhp::SyncNode *to) {
  if (!from || !to || from == to) {
    return;
  }

  auto &succs = m_extra_hb_successors[from];
  if (std::find(succs.begin(), succs.end(), to) == succs.end()) {
    succs.push_back(const_cast<mhp::SyncNode *>(to));
  }
}

void HappensBeforeAnalysis::addExplicitHBPair(const Instruction *from,
                                              const Instruction *to) {
  if (!from || !to || from == to) {
    return;
  }
  m_explicit_hb_pairs.insert({from, to});
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectThreadPrefixInstructions(
    const Instruction *inst) const {
  std::vector<const Instruction *> ordered;
  if (!inst || isInstructionThreadAmbiguous(inst)) {
    return ordered;
  }

  mhp::ThreadID tid = m_mhp.getThreadID(inst);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  for (mhp::SyncNode *node : tfg.getNodes(inst, tid)) {
    worklist.push_back(node);
    visited.insert(node);
  }

  std::set<const Instruction *> collected;
  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (const Instruction *current_inst = current->getInstruction()) {
      collected.insert(current_inst);
    }
    for (mhp::SyncNode *pred : current->getPredecessors()) {
      if (pred->getThreadID() != tid) {
        continue;
      }
      if (visited.insert(pred).second) {
        worklist.push_back(pred);
      }
    }
  }

  ordered.assign(collected.begin(), collected.end());
  return ordered;
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectThreadSuffixInstructions(
    const Instruction *inst) const {
  std::vector<const Instruction *> ordered;
  if (!inst || isInstructionThreadAmbiguous(inst)) {
    return ordered;
  }

  mhp::ThreadID tid = m_mhp.getThreadID(inst);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  for (mhp::SyncNode *node : tfg.getNodes(inst, tid)) {
    worklist.push_back(node);
    visited.insert(node);
  }

  std::set<const Instruction *> collected;
  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (const Instruction *current_inst = current->getInstruction()) {
      collected.insert(current_inst);
    }
    for (mhp::SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() != tid) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  ordered.assign(collected.begin(), collected.end());
  return ordered;
}

std::vector<const Instruction *>
HappensBeforeAnalysis::collectHBRelevantSuffixInstructions(
    const Instruction *inst) const {
  std::vector<const Instruction *> ordered;
  if (!inst || isInstructionThreadAmbiguous(inst)) {
    return ordered;
  }

  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();
  const bool barrier_wait_like = threadAPI && threadAPI->isTDBarWait(inst);
  auto hasInterveningBarrierWait = [&](const Instruction *candidate) {
    if (!barrier_wait_like || !candidate) {
      return false;
    }
    if (inst->getFunction() != candidate->getFunction() ||
        inst->getParent() != candidate->getParent()) {
      return false;
    }
    bool seen_sync = false;
    for (const Instruction &cursor : *inst->getParent()) {
      if (&cursor == inst) {
        seen_sync = true;
        continue;
      }
      if (!seen_sync) {
        continue;
      }
      if (&cursor == candidate) {
        return false;
      }
      if (threadAPI->isTDBarWait(&cursor)) {
        return true;
      }
    }
    return false;
  };

  std::set<const Instruction *> collected;
  if (barrier_wait_like && inst->getParent()) {
    bool stopped_at_next_wait = false;
    for (const Instruction *cursor = inst->getNextNode(); cursor;
         cursor = cursor->getNextNode()) {
      if (threadAPI->isTDBarWait(cursor)) {
        stopped_at_next_wait = true;
        break;
      }
      collected.insert(cursor);
    }
    if (stopped_at_next_wait) {
      ordered.assign(collected.begin(), collected.end());
      return ordered;
    }
    collected.clear();
  }
  if (const BasicBlock *sync_successor = getWitnessSuccessor(inst)) {
    std::deque<const BasicBlock *> worklist;
    std::unordered_set<const BasicBlock *> visited;
    worklist.push_back(sync_successor);
    visited.insert(sync_successor);

    while (!worklist.empty()) {
      const BasicBlock *current = worklist.front();
      worklist.pop_front();
      for (const Instruction &candidate : *current) {
        collected.insert(&candidate);
      }
      for (const BasicBlock *succ : successors(current)) {
        if (visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  } else {
    mhp::ThreadID tid = m_mhp.getThreadID(inst);
    const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
    for (const Instruction *candidate : collectThreadSuffixInstructions(inst)) {
      if (!candidate || candidate == inst) {
        continue;
      }
      if (hasInterveningBarrierWait(candidate)) {
        continue;
      }
      if (!isPostSyncInstruction(inst, candidate)) {
        continue;
      }
      if (tfg.getNodes(candidate, tid).empty()) {
        continue;
      }
      collected.insert(candidate);
    }
  }

  ordered.assign(collected.begin(), collected.end());
  return ordered;
}

bool HappensBeforeAnalysis::isPostSyncInstruction(
    const Instruction *sync_inst, const Instruction *candidate) const {
  if (!sync_inst || !candidate || sync_inst == candidate) {
    return false;
  }
  if (sync_inst->getFunction() != candidate->getFunction()) {
    return true;
  }
  if (sync_inst->getParent() == candidate->getParent()) {
    bool seen_sync = false;
    for (const Instruction &inst : *sync_inst->getParent()) {
      if (&inst == sync_inst) {
        seen_sync = true;
        continue;
      }
      if (&inst == candidate) {
        return seen_sync;
      }
    }
    return false;
  }

  const llvm::PostDominatorTree &pdt =
      getPostDominatorTree(sync_inst->getFunction());
  return pdt.dominates(candidate->getParent(), sync_inst->getParent());
}

void HappensBeforeAnalysis::addExplicitHBClosure(const Instruction *from,
                                                 const Instruction *to) {
  for (const Instruction *prefix : collectThreadPrefixInstructions(from)) {
    addExplicitHBPair(prefix, to);
    for (const Instruction *suffix : collectHBRelevantSuffixInstructions(to)) {
      addExplicitHBPair(prefix, suffix);
    }
  }
}

void HappensBeforeAnalysis::computeAtomicHappensBefore() {
  errs() << "Computing Atomic Happens-Before...\n";

  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (CppAtomics::isAtomic(&*I)) {
        m_atomic_instructions.push_back(&*I);
      }
    }
  }

  errs() << "Deferred " << m_atomic_instructions.size()
         << " atomic/fence operations for witness-aware lowering.\n";
}

void HappensBeforeAnalysis::buildSynchronizesWith() {
  using namespace CppAtomics;

  struct AtomicEvent {
    const Instruction *inst = nullptr;
    AtomicLocationKey location;
    mhp::ThreadID tid = std::numeric_limits<mhp::ThreadID>::max();
    bool is_store_like = false;
    bool is_load_like = false;
    bool is_rmw = false;
    bool is_cmpxchg = false;
    bool has_success_witness = true;
    bool has_release = false;
    bool has_acquire = false;
    bool is_fence = false;
  };

  struct AtomicTarget {
    const Instruction *anchor = nullptr;
    const Instruction *target = nullptr;
    bool is_fence_target = false;
  };

  std::vector<const Instruction *> release_ops;
  std::vector<const Instruction *> acquire_ops;
  std::vector<const Instruction *> promise_sets;
  std::vector<const Instruction *> future_gets;
  std::vector<const Instruction *> call_once_ops;
  std::vector<const Instruction *> async_launches;
  std::vector<const Instruction *> latch_countdowns;
  std::vector<const Instruction *> latch_waits;
  std::vector<const Instruction *> barrier_arrives;
  std::vector<const Instruction *> barrier_waits;
  std::vector<const Instruction *> lock_acquires;
  std::vector<const Instruction *> lock_releases;
  std::vector<const Instruction *> cond_waits;
  std::vector<const Instruction *> cond_signals;
  std::vector<const Instruction *> omp_task_ops;
  std::vector<AtomicEvent> atomic_events;
  std::unordered_map<AtomicLocationKey, std::vector<size_t>,
                     AtomicLocationKeyHash>
      events_by_location;
  std::unordered_map<const Instruction *, const Instruction *>
      release_fence_anchor;
  std::unordered_map<const Instruction *, const Instruction *>
      acquire_fence_anchor;
  std::unordered_map<const Instruction *, size_t> barrier_phase_index;
  std::unordered_map<const Value *, size_t> barrier_expected_counts;

  std::set<std::pair<const Instruction *, const Instruction *>> seen_sync_edges;
  auto addSyncEdge = [&](const Instruction *from, const Instruction *to,
                         const Instruction *suffix_anchor = nullptr) {
    if (!from || !to || from == to) {
      return;
    }
    if (seen_sync_edges.emplace(from, to).second) {
      m_sync_with.emplace_back(from, to);
      if (!suffix_anchor) {
        addExplicitHBClosure(from, to);
        return;
      }

      for (const Instruction *prefix : collectThreadPrefixInstructions(from)) {
        addExplicitHBPair(prefix, to);
        for (const Instruction *suffix :
             collectHBRelevantSuffixInstructions(suffix_anchor)) {
          addExplicitHBPair(prefix, suffix);
        }
      }
    }
  };

  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();
  auto hb_call_graph = std::make_unique<CallGraph>(m_module);
  concurrency::ThreadMultiplicityAnalysis site_multiplicity(
      m_module, hb_call_graph.get());
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  auto isSingleExecutionSite = [&](const Instruction *inst) {
    if (!inst || isInstructionThreadAmbiguous(inst) ||
        site_multiplicity.instructionMayExecuteMultipleTimes(inst)) {
      return false;
    }
    const mhp::ThreadID tid = m_mhp.getThreadID(inst);
    if (tid == std::numeric_limits<mhp::ThreadID>::max()) {
      return false;
    }
    return tfg.getNodes(inst, tid).size() == 1;
  };
  auto sameLockIdentity = [&](const Instruction *lhs,
                              const Instruction *rhs) -> bool {
    if (!lhs || !rhs) {
      return false;
    }
    const Value *lhs_lock = threadAPI->getAnalysisLockIdentity(lhs);
    const Value *rhs_lock = threadAPI->getAnalysisLockIdentity(rhs);
    if (!lhs_lock || !rhs_lock) {
      return false;
    }
    lhs_lock = lhs_lock->stripPointerCasts();
    rhs_lock = rhs_lock->stripPointerCasts();
    if (lhs_lock == rhs_lock) {
      return true;
    }
    return m_alias_analysis && m_alias_analysis->mustAlias(lhs_lock, rhs_lock);
  };
  auto sameCondIdentity = [&](const Instruction *lhs,
                              const Instruction *rhs) -> bool {
    if (!lhs || !rhs) {
      return false;
    }
    const Value *lhs_cond = threadAPI->getCondVal(lhs);
    const Value *rhs_cond = threadAPI->getCondVal(rhs);
    if (!lhs_cond || !rhs_cond) {
      return false;
    }
    lhs_cond = lhs_cond->stripPointerCasts();
    rhs_cond = rhs_cond->stripPointerCasts();
    if (lhs_cond == rhs_cond) {
      return true;
    }
    return m_alias_analysis && m_alias_analysis->mustAlias(lhs_cond, rhs_cond);
  };
  auto findCallOnceCallable = [&](const CallBase *call) -> const Function * {
    if (!call) {
      return nullptr;
    }
    for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
      const Value *arg = call->getArgOperand(idx);
      if (!arg) {
        continue;
      }
      arg = arg->stripPointerCasts();
      if (const auto *func = dyn_cast<Function>(arg)) {
        return func->isDeclaration() ? nullptr : func;
      }
    }
    return nullptr;
  };
  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;

      if (isAtomic(inst)) {
        AtomicEvent event;
        event.inst = inst;
        event.location = getExactAtomicLocation(inst);
        event.tid = m_mhp.getThreadID(inst);
        event.is_store_like = isStore(inst);
        event.is_load_like = isLoad(inst);
        event.is_rmw = isReadModifyWrite(inst);
        event.is_cmpxchg = isa<AtomicCmpXchgInst>(inst);
        event.has_success_witness =
            !event.is_cmpxchg || hasCmpXchgSuccessWitness(inst);
        event.has_release = hasReleaseSemantics(inst);
        event.has_acquire = hasAcquireSemantics(inst);
        event.is_fence = false;
        atomic_events.push_back(event);
        if (event.location.base) {
          events_by_location[event.location].push_back(atomic_events.size() -
                                                       1);
        }

        if (hasReleaseSemantics(inst) &&
            (isStore(inst) || isReadModifyWrite(inst))) {
          release_ops.push_back(inst);
        }
        if (hasAcquireSemantics(inst) &&
            (isLoad(inst) || isReadModifyWrite(inst))) {
          acquire_ops.push_back(inst);
        }
      }

      if (isFenceRelease(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst)) {
        release_ops.push_back(inst);
      }
      if (isFenceAcquire(inst) || isFenceAcqRel(inst) || isFenceSeqCst(inst)) {
        acquire_ops.push_back(inst);
      }

      const CallBase *call = dyn_cast<CallBase>(inst);
      if (!call) {
        continue;
      }

      const Function *callee = threadAPI->getCallee(call);
      ThreadAPI::TD_TYPE type = threadAPI->getType(call);

      if (type == ThreadAPI::TD_BAR_INIT && call->arg_size() >= 3) {
        if (const Value *barrier = threadAPI->getBarrierVal(inst)) {
          if (const auto *count =
                  dyn_cast<ConstantInt>(call->getArgOperand(2))) {
            barrier_expected_counts[barrier->stripPointerCasts()] =
                static_cast<size_t>(count->getZExtValue());
          }
        }
      }

      if (callee && callee->getName().contains("get_future") &&
          call->arg_size() >= 1) {
        const Value *promise_obj = traceSharedState(call->getArgOperand(0));
        if (promise_obj) {
          m_future_shared_state[call] = promise_obj;
        }
      }
      if (type == ThreadAPI::TD_ASYNC && threadAPI->isForkLike(call)) {
        // Use the async call result as the future shared-state witness so
        // future::get()/wait() can recover the associated spawned task.
        m_future_shared_state[call] = call;
      }

      switch (type) {
      case ThreadAPI::TD_ACQUIRE:
      case ThreadAPI::TD_RWLOCK_WRLOCK:
      case ThreadAPI::TD_SHARED_WRLOCK:
      case ThreadAPI::TD_LOCK_GUARD_CTOR:
      case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
      case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
      case ThreadAPI::TD_SCOPED_LOCK_CTOR:
      case ThreadAPI::TD_KERNEL_SPIN_LOCK:
      case ThreadAPI::TD_KERNEL_MUTEX_LOCK:
      case ThreadAPI::TD_KERNEL_DOWN:
      case ThreadAPI::TD_KERNEL_WRITE_LOCK:
      case ThreadAPI::TD_KERNEL_DOWN_WRITE:
        if (!threadAPI->isTryLock(inst) && !threadAPI->isSemaphoreOp(inst)) {
          lock_acquires.push_back(inst);
        }
        break;
      case ThreadAPI::TD_RELEASE:
      case ThreadAPI::TD_LOCK_GUARD_DTOR:
      case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
      case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
      case ThreadAPI::TD_SCOPED_LOCK_DTOR:
      case ThreadAPI::TD_KERNEL_SPIN_UNLOCK:
      case ThreadAPI::TD_KERNEL_MUTEX_UNLOCK:
      case ThreadAPI::TD_KERNEL_UP:
      case ThreadAPI::TD_KERNEL_WRITE_UNLOCK:
      case ThreadAPI::TD_KERNEL_UP_WRITE:
        if (!threadAPI->isSemaphoreOp(inst)) {
          lock_releases.push_back(inst);
        }
        break;
      case ThreadAPI::TD_COND_WAIT:
        cond_waits.push_back(inst);
        break;
      case ThreadAPI::TD_COND_SIGNAL:
      case ThreadAPI::TD_COND_BROADCAST:
        cond_signals.push_back(inst);
        break;
      case ThreadAPI::TD_PROMISE_SET:
        promise_sets.push_back(inst);
        break;
      case ThreadAPI::TD_FUTURE_GET:
      case ThreadAPI::TD_FUTURE_WAIT:
        future_gets.push_back(inst);
        break;
      case ThreadAPI::TD_CALL_ONCE:
        call_once_ops.push_back(inst);
        break;
      case ThreadAPI::TD_ASYNC:
        if (threadAPI->isForkLike(call)) {
          async_launches.push_back(inst);
        }
        break;
      case ThreadAPI::TD_LATCH_COUNT_DOWN:
      case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
        latch_countdowns.push_back(inst);
        if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT) {
          latch_waits.push_back(inst);
        }
        break;
      case ThreadAPI::TD_LATCH_WAIT:
        latch_waits.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
        barrier_arrives.push_back(inst);
        barrier_waits.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_ARRIVE:
        barrier_arrives.push_back(inst);
        break;
      case ThreadAPI::TD_BARRIER_WAIT_CPP20:
        barrier_waits.push_back(inst);
        break;
      case ThreadAPI::TD_OMP_TASK:
      case ThreadAPI::TD_OMP_TASKWAIT:
      case ThreadAPI::TD_OMP_TASKWAIT_DEPS:
      case ThreadAPI::TD_OMP_TASKYIELD:
      case ThreadAPI::TD_OMP_TASKGROUP_START:
      case ThreadAPI::TD_OMP_TASKGROUP_END:
      case ThreadAPI::TD_OMP_TASK_WITH_DEPS:
      case ThreadAPI::TD_OMP_TASKLOOP:
      case ThreadAPI::TD_OMP_TASK_COMPLETE:
      case ThreadAPI::TD_OMP_SINGLE_START:
      case ThreadAPI::TD_OMP_SINGLE_END:
      case ThreadAPI::TD_OMP_MASTER_START:
      case ThreadAPI::TD_OMP_MASTER_END:
      case ThreadAPI::TD_OMP_ORDERED_START:
      case ThreadAPI::TD_OMP_ORDERED_END:
      case ThreadAPI::TD_OMP_REDUCE_START:
      case ThreadAPI::TD_OMP_REDUCE_END:
      case ThreadAPI::TD_OMP_REDUCE_NOWAIT_START:
      case ThreadAPI::TD_OMP_REDUCE_NOWAIT_END:
      case ThreadAPI::TD_OMP_FOR_STATIC_INIT:
      case ThreadAPI::TD_OMP_FOR_STATIC_FINI:
      case ThreadAPI::TD_OMP_FOR_DISPATCH_INIT:
      case ThreadAPI::TD_OMP_FOR_DISPATCH_NEXT:
      case ThreadAPI::TD_OMP_FOR_DISPATCH_FINI:
      case ThreadAPI::TD_OMP_FLUSH:
      case ThreadAPI::TD_OMP_DOACROSS_SUBMIT:
        omp_task_ops.push_back(inst);
        break;
      default:
        break;
      }
    }
  }

  for (const Instruction *P : promise_sets) {
    for (const Instruction *F : future_gets) {
      if (samePromiseFuturePair(P, F)) {
        addSyncEdge(P, F);
      }
    }
  }

  auto sameAsyncFuturePair = [&](const Instruction *async_launch,
                                 const Instruction *future_wait) {
    const CallBase *future_cb = dyn_cast<CallBase>(future_wait);
    if (!async_launch || !future_cb || future_cb->arg_size() == 0) {
      return false;
    }
    const Value *future_state = traceSharedState(future_cb->getArgOperand(0));
    return future_state == async_launch;
  };

  for (const Instruction *async_launch : async_launches) {
    mhp::ThreadID async_tid = 0;
    mhp::ThreadID launch_tid = m_mhp.getThreadID(async_launch);
    const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
    std::vector<mhp::SyncNode *> launch_nodes =
        launch_tid == std::numeric_limits<mhp::ThreadID>::max()
            ? tfg.getNodes(async_launch)
            : tfg.getNodes(async_launch, launch_tid);
    for (mhp::SyncNode *launch_node : launch_nodes) {
      if (launch_node && launch_node->getForkedThread() != 0) {
        async_tid = launch_node->getForkedThread();
        break;
      }
    }
    if (!async_tid) {
      continue;
    }

    std::vector<mhp::SyncNode *> async_exits =
        tfg.getThreadExitNodes(async_tid);
    if (async_exits.empty()) {
      continue;
    }

    for (const Instruction *future_wait : future_gets) {
      if (!sameAsyncFuturePair(async_launch, future_wait)) {
        continue;
      }
      for (const mhp::SyncNode *async_exit : async_exits) {
        const Instruction *async_exit_inst =
            async_exit ? async_exit->getInstruction() : nullptr;
        if (async_exit_inst) {
          addSyncEdge(async_exit_inst, future_wait);
        }
      }
    }
  }

  size_t direct_atomic_edges = 0;
  size_t deferred_direct_atomic_relations = 0;
  size_t mixed_fence_atomic_edges = 0;
  size_t deferred_mixed_fence_relations = 0;
  size_t release_sequence_edges = 0;
  size_t deferred_release_sequence_relations = 0;
  for (const AtomicEvent &event : atomic_events) {
    if (!event.location.base || isInstructionThreadAmbiguous(event.inst)) {
      continue;
    }

    if (event.is_store_like) {
      if (const Instruction *fence = getSinglePrecedingReleaseFence(event.inst)) {
        release_fence_anchor[fence] = event.inst;
      }
    }

    if (event.is_load_like) {
      if (const Instruction *fence =
              getSingleFollowingAcquireFence(event.inst)) {
        acquire_fence_anchor[fence] = event.inst;
      }
    }
  }

  auto getReleaseCandidates = [&](const AtomicLocationKey &location) {
    std::vector<std::pair<const Instruction *, const Instruction *>> candidates;
    if (!location.base) {
      return candidates;
    }

    auto loc_it = events_by_location.find(location);
    if (loc_it == events_by_location.end() || loc_it->second.empty()) {
      return candidates;
    }
    const Instruction *representative =
        atomic_events[loc_it->second.front()].inst;

    const AtomicEvent *release_head = nullptr;
    std::vector<const AtomicEvent *> release_rmw_events;
    std::vector<const AtomicEvent *> release_sequence_tails;
    bool has_non_release_store_like = false;
    for (const auto &bucket : events_by_location) {
      if (bucket.second.empty()) {
        continue;
      }
      const Instruction *bucket_rep = atomic_events[bucket.second.front()].inst;
      if (representative != bucket_rep &&
          !sameAtomicLocation(representative, bucket_rep)) {
        continue;
      }
      for (size_t idx : bucket.second) {
        const AtomicEvent &event = atomic_events[idx];
        if (!event.is_store_like ||
            event.tid == std::numeric_limits<mhp::ThreadID>::max()) {
          continue;
        }
        if (!event.has_release) {
          if (event.is_rmw) {
            release_sequence_tails.push_back(&event);
            continue;
          }
          has_non_release_store_like = true;
          continue;
        }
        if (event.is_cmpxchg && !event.has_success_witness) {
          ++deferred_direct_atomic_relations;
          ++m_deferred_sync_counts
              ["atomic_cmpxchg_release_missing_success_witness"];
          continue;
        }
        if (event.is_rmw) {
          release_rmw_events.push_back(&event);
          continue;
        }
        if (release_head) {
          candidates.clear();
          return candidates;
        }
        release_head = &event;
      }
    }

    if (has_non_release_store_like &&
        (release_head != nullptr || !release_rmw_events.empty() ||
         !release_sequence_tails.empty())) {
      ++m_deferred_sync_counts["atomic_nonrelease_store_competes_with_release"];
      candidates.clear();
      return candidates;
    }

    const AtomicEvent *effective_release_head = release_head;
    if (effective_release_head) {
      for (const AtomicEvent *release_rmw : release_rmw_events) {
        if (hasProgramOrder(release_rmw->inst, effective_release_head->inst)) {
          candidates.clear();
          return candidates;
        }
        release_sequence_tails.push_back(release_rmw);
      }
    } else {
      if (release_rmw_events.size() == 1) {
        effective_release_head = release_rmw_events.front();
      } else if (release_rmw_events.size() > 1) {
        candidates.clear();
        return candidates;
      }
    }

    if (effective_release_head) {
      for (const AtomicEvent *rmw_candidate : release_sequence_tails) {
        if (hasProgramOrder(rmw_candidate->inst,
                            effective_release_head->inst)) {
          candidates.clear();
          return candidates;
        }
      }
    }

    for (const auto &entry : release_fence_anchor) {
      if (!sameAtomicLocation(entry.second, representative)) {
        continue;
      }
      if (effective_release_head || !candidates.empty()) {
        candidates.clear();
        return candidates;
      }
      candidates.emplace_back(entry.first, entry.second);
    }

    if (effective_release_head) {
      candidates.emplace_back(effective_release_head->inst,
                              effective_release_head->inst);
    }

    return candidates;
  };

  auto getAcquireTargets = [&](const AtomicLocationKey &location) {
    std::vector<AtomicTarget> targets;
    auto loc_it = events_by_location.find(location);
    if (loc_it == events_by_location.end() || loc_it->second.empty()) {
      return targets;
    }
    const Instruction *representative =
        atomic_events[loc_it->second.front()].inst;

    for (const auto &bucket : events_by_location) {
      if (bucket.second.empty()) {
        continue;
      }
      const Instruction *bucket_rep = atomic_events[bucket.second.front()].inst;
      if (representative != bucket_rep &&
          !sameAtomicLocation(representative, bucket_rep)) {
        continue;
      }
      for (size_t idx : bucket.second) {
        const AtomicEvent &event = atomic_events[idx];
        if (!event.is_load_like || !event.has_acquire ||
            event.tid == std::numeric_limits<mhp::ThreadID>::max()) {
          continue;
        }
        if (event.is_cmpxchg && !event.has_success_witness) {
          ++deferred_direct_atomic_relations;
          ++m_deferred_sync_counts
              ["atomic_cmpxchg_acquire_missing_success_witness"];
          continue;
        }
        if (!hasBranchWitness(event.inst) && !hasAssumeWitness(event.inst)) {
          ++deferred_direct_atomic_relations;
          ++m_deferred_sync_counts["atomic_direct_missing_branch_witness"];
          continue;
        }
        targets.push_back({event.inst, event.inst, false});
      }
    }

    for (const auto &entry : acquire_fence_anchor) {
      if (!sameAtomicLocation(entry.second, representative)) {
        continue;
      }
      if (!hasBranchWitness(entry.second) && !hasAssumeWitness(entry.second)) {
        ++deferred_mixed_fence_relations;
        ++m_deferred_sync_counts["atomic_mixed_fence_missing_branch_witness"];
        continue;
      }
      targets.push_back({entry.first, entry.second, true});
    }

    return targets;
  };

  {
    auto getBarrierPhaseKey = [&](const Instruction *inst) {
      const Value *identity = threadAPI->getBarrierVal(inst);
      identity = identity ? identity->stripPointerCasts() : nullptr;
      return std::make_pair(identity ? identity
                                     : static_cast<const Value *>(inst),
                            m_mhp.getThreadID(inst));
    };

    std::map<std::pair<const Value *, mhp::ThreadID>, size_t> next_phase;
    std::map<std::pair<const Value *, mhp::ThreadID>, size_t> pending_arrive;

    for (Function &F : m_module) {
      if (F.isDeclaration()) {
        continue;
      }
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        const Instruction *inst = &*I;
        const auto *call = dyn_cast<CallBase>(inst);
        if (!call) {
          continue;
        }

        const ThreadAPI::TD_TYPE type = threadAPI->getType(call);
        if (type != ThreadAPI::TD_BARRIER_ARRIVE &&
            type != ThreadAPI::TD_BARRIER_WAIT_CPP20 &&
            type != ThreadAPI::TD_BARRIER_ARRIVE_WAIT &&
            type != ThreadAPI::TD_BAR_WAIT) {
          continue;
        }

        mhp::ThreadID tid = m_mhp.getThreadID(inst);
        if (tid == std::numeric_limits<mhp::ThreadID>::max()) {
          continue;
        }

        auto key = getBarrierPhaseKey(inst);
        size_t &phase_cursor = next_phase[key];
        size_t phase = phase_cursor;

        if (type == ThreadAPI::TD_BARRIER_ARRIVE) {
          pending_arrive[key] = phase;
        } else if (type == ThreadAPI::TD_BARRIER_WAIT_CPP20) {
          auto pending_it = pending_arrive.find(key);
          if (pending_it != pending_arrive.end()) {
            phase = pending_it->second;
            pending_arrive.erase(pending_it);
            phase_cursor = std::max(phase_cursor, phase + 1);
          } else {
            phase = phase_cursor++;
          }
        } else {
          pending_arrive.erase(key);
          phase = phase_cursor++;
        }

        barrier_phase_index[inst] = phase;
      }
    }
  }

  for (const auto &entry : events_by_location) {
    const AtomicLocationKey &location = entry.first;
    const Instruction *representative =
        atomic_events[entry.second.front()].inst;
    auto release_candidates = getReleaseCandidates(location);
    if (release_candidates.size() != 1) {
      ++m_deferred_sync_counts["atomic_release_candidate_unresolved"];
      for (size_t idx : entry.second) {
        const AtomicEvent &event = atomic_events[idx];
        if (event.is_load_like && event.has_acquire) {
          ++deferred_direct_atomic_relations;
        }
      }
      continue;
    }

    bool has_release_sequence = false;
    std::vector<const AtomicEvent *> release_sequence_events;
    for (size_t idx : entry.second) {
      const AtomicEvent &event = atomic_events[idx];
      if (event.inst != release_candidates.front().second &&
          event.is_store_like && event.is_rmw &&
          (!event.is_cmpxchg || event.has_success_witness)) {
        has_release_sequence = true;
        release_sequence_events.push_back(&event);
      }
    }

    std::vector<AtomicTarget> targets = getAcquireTargets(location);

    for (const AtomicTarget &target : targets) {
      const Instruction *sync_target =
          target.is_fence_target ? target.anchor : target.target;
      if (!sync_target) {
        continue;
      }
      if (m_mhp.getThreadID(release_candidates.front().first) ==
          m_mhp.getThreadID(sync_target)) {
        continue;
      }

      bool supported_release_sequence = false;
      if (has_release_sequence) {
        if (release_sequence_events.empty()) {
          ++deferred_release_sequence_relations;
          ++m_deferred_sync_counts["atomic_release_sequence_tail_ambiguous"];
          continue;
        }
        const ConstantInt *initial_value =
            getAtomicInitialConstant(release_candidates.front().second);
        const ConstantInt *sequence_value =
            getAtomicStoredConstant(release_candidates.front().second);
        for (const AtomicEvent *event : release_sequence_events) {
          const auto *rmw = dyn_cast<AtomicRMWInst>(event->inst);
          sequence_value = evaluateAtomicRMWValue(rmw, sequence_value);
          if (!sequence_value) {
            break;
          }
        }

        if (!target.is_fence_target && sequence_value &&
            candidateMatchesAcquireWitness(sequence_value, target.target,
                                           initial_value)) {
          supported_release_sequence = true;
        } else {
          ++deferred_release_sequence_relations;
          ++m_deferred_sync_counts
              ["atomic_release_sequence_requires_reads_from_proof"];
          continue;
        }
      } else if (!target.is_fence_target &&
                 !releaseMatchesAcquireWitness(
                     release_candidates.front().second, target.target)) {
        ++deferred_direct_atomic_relations;
        ++m_deferred_sync_counts["atomic_witness_value_incompatible"];
        continue;
      }

      size_t before = m_sync_with.size();
      addSyncEdge(release_candidates.front().first, sync_target,
                  target.is_fence_target ? target.target : nullptr);
      bool modeled = m_sync_with.size() != before;
      if (!modeled) {
        ++m_deferred_sync_counts["atomic_release_candidate_unresolved"];
        continue;
      }
      if (supported_release_sequence) {
        ++release_sequence_edges;
        ++m_deferred_sync_counts["atomic_release_sequence_edges_modeled"];
      } else if (target.is_fence_target ||
                 CppAtomics::isFence(release_candidates.front().first)) {
        ++mixed_fence_atomic_edges;
        ++m_deferred_sync_counts["atomic_mixed_fence_edges_modeled"];
      } else {
        ++direct_atomic_edges;
        ++m_deferred_sync_counts["atomic_direct_edges_modeled"];
      }
    }
  }

  size_t call_once_edges = 0;
  size_t deferred_call_once_relations = 0;
  for (size_t i = 0; i < call_once_ops.size(); ++i) {
    for (size_t j = i + 1; j < call_once_ops.size(); ++j) {
      if (!sameOnceFlag(call_once_ops[i], call_once_ops[j])) {
        continue;
      }
      const auto *source_cb_i = dyn_cast<CallBase>(call_once_ops[i]);
      const auto *source_cb_j = dyn_cast<CallBase>(call_once_ops[j]);
      const Function *callable_i = findCallOnceCallable(source_cb_i);
      const Function *callable_j = findCallOnceCallable(source_cb_j);
      if (!callable_i || !callable_j || callable_i != callable_j) {
        ++deferred_call_once_relations;
        continue;
      }
      bool emitted = false;
      const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
      auto wireCallOnceSite = [&](const Instruction *source_call_once,
                                  const Instruction *target_call_once) {
        const auto *source_cb = dyn_cast<CallBase>(source_call_once);
        const Function *callable = findCallOnceCallable(source_cb);
        if (!callable || isInstructionThreadAmbiguous(source_call_once) ||
            isInstructionThreadAmbiguous(target_call_once)) {
          return;
        }

        const mhp::ThreadID source_tid = m_mhp.getThreadID(source_call_once);
        const mhp::ThreadID target_tid = m_mhp.getThreadID(target_call_once);
        std::vector<mhp::SyncNode *> source_nodes =
            tfg.getNodes(source_call_once, source_tid);
        std::vector<mhp::SyncNode *> target_nodes =
            tfg.getNodes(target_call_once, target_tid);
        if (source_nodes.empty() || target_nodes.empty()) {
          return;
        }

        for (const Instruction &callback_inst : instructions(*callable)) {
          for (mhp::SyncNode *source_node : source_nodes) {
            const mhp::CallContextID callback_ctx = source_node->getNodeID();
            if (mhp::SyncNode *callback_node =
                    tfg.getNode(&callback_inst, source_tid, callback_ctx)) {
              for (mhp::SyncNode *target_node : target_nodes) {
                for (const Instruction *suffix :
                     collectThreadSuffixInstructions(target_call_once)) {
                  if (suffix == target_call_once) {
                    continue;
                  }
                  for (mhp::SyncNode *suffix_node :
                       tfg.getNodes(suffix, target_tid)) {
                    if (suffix_node->getCallContextID() !=
                        target_node->getCallContextID()) {
                      continue;
                    }
                    addExtraHBEdge(callback_node, suffix_node);
                    emitted = true;
                  }
                }
              }
            }
          }
        }
      };

      wireCallOnceSite(call_once_ops[i], call_once_ops[i]);
      wireCallOnceSite(call_once_ops[i], call_once_ops[j]);
      wireCallOnceSite(call_once_ops[j], call_once_ops[i]);
      wireCallOnceSite(call_once_ops[j], call_once_ops[j]);
      if (emitted) {
        ++call_once_edges;
      } else {
        ++deferred_call_once_relations;
      }
    }
  }

  size_t latch_edges = 0;
  size_t deferred_latch_relations = 0;
  auto countMatchingLatches =
      [&](const Instruction *inst,
          const std::vector<const Instruction *> &candidates) {
        size_t count = 0;
        for (const Instruction *candidate : candidates) {
          if (sameLatch(inst, candidate)) {
            ++count;
          }
        }
        return count;
      };
  for (const Instruction *countdown : latch_countdowns) {
    for (const Instruction *wait : latch_waits) {
      if (!sameLatch(countdown, wait)) {
        continue;
      }
      if (countMatchingLatches(countdown, latch_countdowns) == 1 &&
          countMatchingLatches(wait, latch_waits) == 1) {
        size_t before = m_sync_with.size();
        addSyncEdge(countdown, wait);
        if (m_sync_with.size() != before) {
          ++latch_edges;
        }
      } else {
        ++deferred_latch_relations;
      }
    }
  }

  size_t barrier_edges = 0;
  size_t deferred_barrier_relations = 0;
  auto hasAmbiguousSplitPhaseBarrier = [&](const Instruction *inst) {
    std::unordered_map<mhp::ThreadID, size_t> arrive_counts;
    std::unordered_map<mhp::ThreadID, size_t> wait_counts;
    std::unordered_set<mhp::ThreadID> distinct_threads;
    auto phase_it = barrier_phase_index.find(inst);
    const bool filter_by_phase = phase_it != barrier_phase_index.end();
    const size_t phase = filter_by_phase ? phase_it->second : 0;
    for (const Instruction *candidate : barrier_arrives) {
      auto candidate_phase_it = barrier_phase_index.find(candidate);
      if (filter_by_phase && candidate_phase_it != barrier_phase_index.end() &&
          candidate_phase_it->second != phase) {
        continue;
      }
      if (sameBarrier(inst, candidate)) {
        ++arrive_counts[m_mhp.getThreadID(candidate)];
        distinct_threads.insert(m_mhp.getThreadID(candidate));
      }
    }
    for (const Instruction *candidate : barrier_waits) {
      auto candidate_phase_it = barrier_phase_index.find(candidate);
      if (filter_by_phase && candidate_phase_it != barrier_phase_index.end() &&
          candidate_phase_it->second != phase) {
        continue;
      }
      if (sameBarrier(inst, candidate)) {
        ++wait_counts[m_mhp.getThreadID(candidate)];
        distinct_threads.insert(m_mhp.getThreadID(candidate));
      }
    }
    const Value *barrier =
        threadAPI->getBarrierVal(inst)
            ? threadAPI->getBarrierVal(inst)->stripPointerCasts()
            : nullptr;
    auto expected_it = barrier_expected_counts.find(barrier);
    if (expected_it != barrier_expected_counts.end() &&
        distinct_threads.size() < expected_it->second) {
      return true;
    }
    for (const auto &entry : arrive_counts) {
      if (entry.second > 1) {
        return true;
      }
    }
    for (const auto &entry : wait_counts) {
      if (entry.second > 1) {
        return true;
      }
    }
    return false;
  };
  for (const Instruction *arrive : barrier_arrives) {
    for (const Instruction *wait : barrier_waits) {
      if (!sameBarrier(arrive, wait) ||
          m_mhp.getThreadID(arrive) == m_mhp.getThreadID(wait)) {
        continue;
      }
      auto arrive_phase_it = barrier_phase_index.find(arrive);
      auto wait_phase_it = barrier_phase_index.find(wait);
      if (arrive_phase_it != barrier_phase_index.end() &&
          wait_phase_it != barrier_phase_index.end() &&
          arrive_phase_it->second != wait_phase_it->second) {
        continue;
      }
      if (!hasAmbiguousSplitPhaseBarrier(arrive) &&
          !hasAmbiguousSplitPhaseBarrier(wait)) {
        size_t before = m_sync_with.size();
        addSyncEdge(arrive, wait);
        if (m_sync_with.size() != before) {
          ++barrier_edges;
        }
      } else {
        ++deferred_barrier_relations;
      }
    }
  }

  size_t mutex_handoff_edges = 0;
  size_t deferred_mutex_relations = 0;
  std::unordered_map<mhp::ThreadID,
                     std::pair<mhp::ThreadID, const Instruction *>>
      fork_parent_by_thread;
  for (mhp::SyncNode *node : tfg.getAllNodes()) {
    if (!node || node->getType() != mhp::SyncNodeType::THREAD_FORK ||
        node->getForkedThread() == 0 || !node->getInstruction()) {
      continue;
    }
    fork_parent_by_thread.emplace(
        node->getForkedThread(),
        std::make_pair(node->getThreadID(), node->getInstruction()));
  }

  for (const Instruction *acquire : lock_acquires) {
    if (!isSingleExecutionSite(acquire)) {
      continue;
    }
    const mhp::ThreadID child_tid = m_mhp.getThreadID(acquire);
    auto fork_it = fork_parent_by_thread.find(child_tid);
    if (fork_it == fork_parent_by_thread.end()) {
      continue;
    }

    const mhp::ThreadID parent_tid = fork_it->second.first;
    const Instruction *fork_inst = fork_it->second.second;
    if (!fork_inst || !isSingleExecutionSite(fork_inst)) {
      ++deferred_mutex_relations;
      continue;
    }

    std::vector<const Instruction *> child_acquires;
    std::vector<const Instruction *> parent_acquires;
    std::vector<const Instruction *> parent_releases;
    for (const Instruction *candidate : lock_acquires) {
      if (!sameLockIdentity(candidate, acquire) ||
          !isSingleExecutionSite(candidate)) {
        continue;
      }
      const mhp::ThreadID candidate_tid = m_mhp.getThreadID(candidate);
      if (candidate_tid == child_tid) {
        child_acquires.push_back(candidate);
      } else if (candidate_tid == parent_tid &&
                 hasProgramOrder(candidate, fork_inst)) {
        parent_acquires.push_back(candidate);
      }
    }
    for (const Instruction *candidate : lock_releases) {
      if (!sameLockIdentity(candidate, acquire) ||
          !isSingleExecutionSite(candidate)) {
        continue;
      }
      if (m_mhp.getThreadID(candidate) == parent_tid &&
          hasProgramOrder(fork_inst, candidate)) {
        parent_releases.push_back(candidate);
      }
    }

    if (child_acquires.size() != 1 || parent_acquires.size() != 1 ||
        parent_releases.size() != 1) {
      ++deferred_mutex_relations;
      continue;
    }

    bool parent_released_before_fork = false;
    for (const Instruction *candidate : lock_releases) {
      if (!sameLockIdentity(candidate, acquire) ||
          m_mhp.getThreadID(candidate) != parent_tid) {
        continue;
      }
      if (hasProgramOrder(parent_acquires.front(), candidate) &&
          hasProgramOrder(candidate, fork_inst)) {
        parent_released_before_fork = true;
        break;
      }
    }
    if (parent_released_before_fork) {
      ++deferred_mutex_relations;
      continue;
    }

    bool parent_reacquired_after_fork = false;
    for (const Instruction *candidate : lock_acquires) {
      if (candidate == parent_acquires.front() ||
          !sameLockIdentity(candidate, acquire) ||
          m_mhp.getThreadID(candidate) != parent_tid) {
        continue;
      }
      if (hasProgramOrder(fork_inst, candidate) &&
          hasProgramOrder(candidate, parent_releases.front())) {
        parent_reacquired_after_fork = true;
        break;
      }
    }
    if (parent_reacquired_after_fork) {
      ++deferred_mutex_relations;
      continue;
    }

    size_t before = m_sync_with.size();
    addSyncEdge(parent_releases.front(), acquire, acquire);
    if (m_sync_with.size() != before) {
      ++mutex_handoff_edges;
    }
  }

  size_t condvar_edges = 0;
  size_t deferred_condvar_relations = 0;
  for (const Instruction *wait : cond_waits) {
    if (!wait || !isSingleExecutionSite(wait)) {
      continue;
    }

    size_t competing_waits = 0;
    for (const Instruction *other_wait : cond_waits) {
      if (other_wait && isSingleExecutionSite(other_wait) &&
          sameCondIdentity(wait, other_wait) &&
          m_mhp.getThreadID(other_wait) != m_mhp.getThreadID(wait)) {
        ++competing_waits;
      }
    }

    std::vector<const Instruction *> matching_signals;
    for (const Instruction *signal : cond_signals) {
      if (!sameCondIdentity(wait, signal)) {
        continue;
      }
      if (!signal || !isSingleExecutionSite(signal) ||
          m_mhp.getThreadID(signal) == m_mhp.getThreadID(wait)) {
        continue;
      }
      matching_signals.push_back(signal);
    }

    if (competing_waits == 0 && matching_signals.size() == 1) {
      size_t before = m_sync_with.size();
      addSyncEdge(matching_signals.front(), wait, wait);
      if (m_sync_with.size() != before) {
        ++condvar_edges;
        continue;
      }
    }

    if (!matching_signals.empty()) {
      ++deferred_condvar_relations;
    }
  }

  size_t omp_task_dependency_edges = 0;
  size_t omp_task_exclusion_relations = 0;
  size_t omp_task_unknown_relations = 0;
  std::unique_ptr<OpenMP::OpenMPSemantics> owned_semantics;
  const OpenMP::OpenMPSemantics *semantics = m_mhp.getOpenMPSemantics();
  if (!semantics) {
    owned_semantics = std::make_unique<OpenMP::OpenMPSemantics>(m_module);
    owned_semantics->analyze();
    semantics = owned_semantics.get();
  }

  for (const auto &entry : semantics->getRelations()) {
    const OpenMP::Task *lhs = entry.first.first;
    const OpenMP::Task *rhs = entry.first.second;
    const concurrency::Relation &relation = entry.second;
    if (!lhs || !rhs || !lhs->task_create || !rhs->task_create) {
      continue;
    }
    if (relation.kind == concurrency::RelationKind::MutuallyExclusive) {
      ++omp_task_exclusion_relations;
      continue;
    }
    if (relation.kind == concurrency::RelationKind::UnknownDueToModelGap) {
      ++omp_task_unknown_relations;
      continue;
    }
    if (relation.kind != concurrency::RelationKind::MustHappenBefore &&
        relation.kind != concurrency::RelationKind::SelectiveHappenBefore) {
      continue;
    }

    size_t before = m_sync_with.size();
    addSyncEdge(lhs->task_create, rhs->task_create);
    if (m_sync_with.size() != before) {
      ++omp_task_dependency_edges;
    }
  }

  m_deferred_sync_counts["atomic_release_candidates"] = release_ops.size();
  m_deferred_sync_counts["atomic_acquire_candidates"] = acquire_ops.size();
  m_deferred_sync_counts["atomic_direct_sync_edges"] = direct_atomic_edges;
  m_deferred_sync_counts["atomic_direct_relations_deferred"] =
      deferred_direct_atomic_relations;
  m_deferred_sync_counts["atomic_release_sequence_sync_edges"] =
      release_sequence_edges;
  m_deferred_sync_counts["atomic_release_sequence_relations_deferred"] =
      deferred_release_sequence_relations;
  m_deferred_sync_counts["atomic_mixed_fence_sync_edges"] =
      mixed_fence_atomic_edges;
  m_deferred_sync_counts["atomic_mixed_fence_relations_deferred"] =
      deferred_mixed_fence_relations;
  m_deferred_sync_counts["call_once_ops"] = call_once_ops.size();
  m_deferred_sync_counts["call_once_sync_edges"] = call_once_edges;
  m_deferred_sync_counts["call_once_relations_deferred"] =
      deferred_call_once_relations;
  m_deferred_sync_counts["latch_ops"] =
      latch_countdowns.size() + latch_waits.size();
  m_deferred_sync_counts["latch_sync_edges"] = latch_edges;
  m_deferred_sync_counts["latch_relations_deferred"] = deferred_latch_relations;
  m_deferred_sync_counts["barrier_ops"] =
      barrier_arrives.size() + barrier_waits.size();
  m_deferred_sync_counts["barrier_sync_edges"] = barrier_edges;
  m_deferred_sync_counts["barrier_relations_deferred"] =
      deferred_barrier_relations;
  m_deferred_sync_counts["mutex_ops"] =
      lock_acquires.size() + lock_releases.size();
  m_deferred_sync_counts["mutex_handoff_edges"] = mutex_handoff_edges;
  m_deferred_sync_counts["mutex_relations_deferred"] = deferred_mutex_relations;
  m_deferred_sync_counts["condvar_ops"] =
      cond_waits.size() + cond_signals.size();
  m_deferred_sync_counts["condvar_sync_edges"] = condvar_edges;
  m_deferred_sync_counts["condvar_relations_deferred"] =
      deferred_condvar_relations;
  m_deferred_sync_counts["omp_task_api_ops"] = omp_task_ops.size();
  m_deferred_sync_counts["omp_task_dependency_edges"] =
      omp_task_dependency_edges;
  m_deferred_sync_counts["omp_task_exclusion_relations"] =
      omp_task_exclusion_relations;
  m_deferred_sync_counts["omp_task_unknown_relations"] =
      omp_task_unknown_relations;
  m_deferred_sync_counts["omp_semantic_entities"] =
      semantics->getSemanticEntities().size();
  m_deferred_sync_counts["omp_semantic_events"] =
      semantics->getSemanticEvents().size();
  for (const auto &entry : semantics->getDeferredReasonCounts()) {
    m_deferred_sync_counts[entry.first] += entry.second;
  }

  errs() << "HB deferred sync candidates: atomics="
         << m_deferred_sync_counts["atomic_release_candidates"] +
                m_deferred_sync_counts["atomic_acquire_candidates"]
         << ", atomic_sync_edges="
         << m_deferred_sync_counts["atomic_direct_sync_edges"]
         << ", call_once=" << m_deferred_sync_counts["call_once_ops"]
         << ", call_once_edges="
         << m_deferred_sync_counts["call_once_sync_edges"]
         << ", latches=" << m_deferred_sync_counts["latch_ops"]
         << ", latch_edges=" << m_deferred_sync_counts["latch_sync_edges"]
         << ", barriers=" << m_deferred_sync_counts["barrier_ops"]
         << ", barrier_edges=" << m_deferred_sync_counts["barrier_sync_edges"]
         << ", mutex_ops=" << m_deferred_sync_counts["mutex_ops"]
         << ", mutex_edges=" << m_deferred_sync_counts["mutex_handoff_edges"]
         << ", condvar_ops=" << m_deferred_sync_counts["condvar_ops"]
         << ", condvar_edges=" << m_deferred_sync_counts["condvar_sync_edges"]
         << ", omp_task_ops=" << m_deferred_sync_counts["omp_task_api_ops"]
         << ", omp_task_edges=" << omp_task_dependency_edges << "\n";
}

bool HappensBeforeAnalysis::canReach(const mhp::SyncNode *start,
                                     const mhp::SyncNode *end) const {
  if (!start || !end) {
    return false;
  }

  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  if (tfg.hasReachabilityIndex()) {
    return tfg.canReach(start, end);
  }

  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  worklist.push_back(start);
  visited.insert(start);

  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (current == end) {
      return true;
    }
    for (mhp::SyncNode *succ : current->getSuccessors()) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::canReachWithHB(const mhp::SyncNode *start,
                                           const mhp::SyncNode *end) const {
  if (!start || !end) {
    return false;
  }

  std::deque<const mhp::SyncNode *> worklist;
  std::unordered_set<const mhp::SyncNode *> visited;
  worklist.push_back(start);
  visited.insert(start);

  while (!worklist.empty()) {
    const mhp::SyncNode *current = worklist.front();
    worklist.pop_front();
    if (current == end) {
      return true;
    }

    for (mhp::SyncNode *succ : current->getSuccessors()) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }

    auto extra_it = m_extra_hb_successors.find(current);
    if (extra_it == m_extra_hb_successors.end()) {
      continue;
    }
    for (const mhp::SyncNode *succ : extra_it->second) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::canReachExplicitHB(const Instruction *from,
                                               const Instruction *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  std::deque<const Instruction *> worklist;
  std::unordered_set<const Instruction *> visited;
  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const Instruction *current = worklist.front();
    worklist.pop_front();
    if (current == to) {
      return true;
    }

    for (const auto &pair : m_explicit_hb_pairs) {
      if (pair.first != current) {
        continue;
      }
      if (visited.insert(pair.second).second) {
        worklist.push_back(pair.second);
      }
    }
  }

  return false;
}

bool HappensBeforeAnalysis::hasProgramOrder(const Instruction *A,
                                            const Instruction *B) const {
  if (!A || !B || A == B) {
    return false;
  }

  mhp::ThreadID a_tid = m_mhp.getThreadID(A);
  mhp::ThreadID b_tid = m_mhp.getThreadID(B);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> start_nodes =
      a_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(A)
          : tfg.getNodes(A, a_tid);
  std::vector<mhp::SyncNode *> end_nodes =
      b_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(B)
          : tfg.getNodes(B, b_tid);
  if (start_nodes.empty() || end_nodes.empty()) {
    return false;
  }

  for (mhp::SyncNode *start : start_nodes) {
    bool reached = false;
    for (mhp::SyncNode *end : end_nodes) {
      if (canReach(start, end)) {
        reached = true;
        break;
      }
    }
    if (!reached) {
      return false;
    }
  }
  return true;
}

bool HappensBeforeAnalysis::isInstructionThreadAmbiguous(
    const Instruction *inst) const {
  return !inst ||
         m_mhp.getThreadID(inst) == std::numeric_limits<mhp::ThreadID>::max();
}

bool HappensBeforeAnalysis::sameAtomicLocation(
    const Instruction *store_inst, const Instruction *load_inst) const {
  if (!store_inst || !load_inst) {
    return false;
  }

  const AtomicLocationKey lhs = getExactAtomicLocation(store_inst);
  const AtomicLocationKey rhs = getExactAtomicLocation(load_inst);
  if (!lhs.base || !rhs.base) {
    return false;
  }
  if (lhs == rhs) {
    return true;
  }
  if (lhs.base == rhs.base && lhs.has_precise_offset &&
      rhs.has_precise_offset) {
    return false;
  }

  const Value *p1 = CppAtomics::getAtomicPointer(store_inst);
  const Value *p2 = CppAtomics::getAtomicPointer(load_inst);
  if (!p1 || !p2) {
    return false;
  }
  p1 = p1->stripPointerCasts();
  p2 = p2->stripPointerCasts();
  if (p1 == p2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(p1, p2);
}

bool HappensBeforeAnalysis::isFenceAnchorCompatibleInstruction(
    const Instruction *inst) const {
  if (!inst) {
    return false;
  }
  if (isa<DbgInfoIntrinsic>(inst) || isa<PHINode>(inst)) {
    return true;
  }
  if (const auto *cb = dyn_cast<CallBase>(inst)) {
    return cb->doesNotAccessMemory();
  }
  return !inst->mayReadOrWriteMemory();
}

const Instruction *HappensBeforeAnalysis::findNearestAtomicInBlock(
    const Instruction *inst, bool search_backward, bool require_load_like,
    bool require_store_like) const {
  if (!inst) {
    return nullptr;
  }
  if (!inst->getParent()) {
    return nullptr;
  }

  const Instruction *cursor = inst;
  while (true) {
    cursor = search_backward ? cursor->getPrevNode() : cursor->getNextNode();
    if (!cursor) {
      return nullptr;
    }
    if (CppAtomics::isFence(cursor)) {
      return nullptr;
    }
    if (CppAtomics::isAtomic(cursor)) {
      if (require_load_like && !CppAtomics::isLoad(cursor)) {
        return nullptr;
      }
      if (require_store_like && !CppAtomics::isStore(cursor)) {
        return nullptr;
      }
      return cursor;
    }
    if (!isFenceAnchorCompatibleInstruction(cursor)) {
      return nullptr;
    }
  }
}

const Instruction *HappensBeforeAnalysis::getSinglePrecedingAtomicLoad(
    const Instruction *inst) const {
  return findNearestAtomicInBlock(inst, true, true, false);
}

const Instruction *HappensBeforeAnalysis::getSinglePrecedingReleaseFence(
    const Instruction *inst) const {
  if (!inst) {
    return nullptr;
  }

  std::deque<const Instruction *> worklist;
  std::unordered_set<const Instruction *> visited;
  const Instruction *fence = nullptr;

  auto enqueue = [&](const Instruction *candidate) {
    if (candidate && visited.insert(candidate).second) {
      worklist.push_back(candidate);
    }
  };

  if (const Instruction *prev = inst->getPrevNode()) {
    enqueue(prev);
  } else {
    for (const BasicBlock *pred : predecessors(inst->getParent())) {
      enqueue(pred->getTerminator());
    }
  }

  while (!worklist.empty()) {
    const Instruction *current = worklist.front();
    worklist.pop_front();

    if (CppAtomics::isFence(current)) {
      if (!(CppAtomics::isFenceRelease(current) ||
            CppAtomics::isFenceAcqRel(current) ||
            CppAtomics::isFenceSeqCst(current))) {
        return nullptr;
      }
      if (!fence) {
        fence = current;
      } else if (fence != current) {
        return nullptr;
      }
      continue;
    }

    if (!isFenceAnchorCompatibleInstruction(current)) {
      return nullptr;
    }

    if (const Instruction *prev = current->getPrevNode()) {
      enqueue(prev);
    } else {
      for (const BasicBlock *pred : predecessors(current->getParent())) {
        enqueue(pred->getTerminator());
      }
    }
  }

  return fence;
}

const Instruction *HappensBeforeAnalysis::getSingleFollowingAcquireFence(
    const Instruction *inst) const {
  if (!inst) {
    return nullptr;
  }

  std::deque<const Instruction *> worklist;
  std::unordered_set<const Instruction *> visited;
  const Instruction *fence = nullptr;

  auto enqueue = [&](const Instruction *candidate) {
    if (candidate && visited.insert(candidate).second) {
      worklist.push_back(candidate);
    }
  };

  if (const Instruction *next = inst->getNextNode()) {
    enqueue(next);
  } else {
    for (const BasicBlock *succ : successors(inst->getParent())) {
      enqueue(&succ->front());
    }
  }

  while (!worklist.empty()) {
    const Instruction *current = worklist.front();
    worklist.pop_front();

    if (CppAtomics::isFence(current)) {
      if (!(CppAtomics::isFenceAcquire(current) ||
            CppAtomics::isFenceAcqRel(current) ||
            CppAtomics::isFenceSeqCst(current))) {
        return nullptr;
      }
      if (!fence) {
        fence = current;
      } else if (fence != current) {
        return nullptr;
      }
      continue;
    }

    if (!isFenceAnchorCompatibleInstruction(current)) {
      return nullptr;
    }

    if (const Instruction *next = current->getNextNode()) {
      enqueue(next);
    } else {
      for (const BasicBlock *succ : successors(current->getParent())) {
        enqueue(&succ->front());
      }
    }
  }
  return fence;
}

const PostDominatorTree &
HappensBeforeAnalysis::getPostDominatorTree(const Function *func) const {
  auto it = m_post_dom_cache.find(func);
  if (it != m_post_dom_cache.end()) {
    return *(it->second);
  }

  auto pdt = std::make_unique<PostDominatorTree>();
  pdt->recalculate(*const_cast<Function *>(func));
  auto *pdt_ptr = pdt.get();
  m_post_dom_cache[func] = std::move(pdt);
  return *pdt_ptr;
}

bool HappensBeforeAnalysis::hasSupportedAtomicWitness(
    const Instruction *inst) const {
  if (!inst) {
    return false;
  }
  if (hasBranchWitness(inst)) {
    return true;
  }
  if (const Instruction *fence = getSingleFollowingAcquireFence(inst)) {
    return hasBranchWitness(fence);
  }
  return false;
}

bool HappensBeforeAnalysis::samePromiseFuturePair(
    const Instruction *promise, const Instruction *future) const {
  const CallBase *p = dyn_cast<CallBase>(promise);
  const CallBase *f = dyn_cast<CallBase>(future);
  if (!p || !f || p->arg_size() == 0 || f->arg_size() == 0) {
    return false;
  }

  const Value *promise_base = p->getArgOperand(0)->stripPointerCasts();
  const Value *future_base = f->getArgOperand(0)->stripPointerCasts();
  const Value *promise_state = traceSharedState(promise_base);
  const Value *future_state = traceSharedState(future_base);
  if (!promise_state || !future_state) {
    return false;
  }
  if (promise_state == future_state) {
    return true;
  }
  return m_alias_analysis &&
         m_alias_analysis->mustAlias(promise_state, future_state);
}

bool HappensBeforeAnalysis::sameOnceFlag(const Instruction *call1,
                                         const Instruction *call2) const {
  const CallBase *c1 = dyn_cast<CallBase>(call1);
  const CallBase *c2 = dyn_cast<CallBase>(call2);
  if (!c1 || !c2 || c1->arg_size() < 1 || c2->arg_size() < 1) {
    return false;
  }

  const Value *flag1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *flag2 = c2->getArgOperand(0)->stripPointerCasts();
  if (flag1 == flag2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(flag1, flag2);
}

bool HappensBeforeAnalysis::sameLatch(const Instruction *inst1,
                                      const Instruction *inst2) const {
  const CallBase *c1 = dyn_cast<CallBase>(inst1);
  const CallBase *c2 = dyn_cast<CallBase>(inst2);
  if (!c1 || !c2 || c1->arg_size() < 1 || c2->arg_size() < 1) {
    return false;
  }

  const Value *latch1 = c1->getArgOperand(0)->stripPointerCasts();
  const Value *latch2 = c2->getArgOperand(0)->stripPointerCasts();
  if (latch1 == latch2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(latch1, latch2);
}

bool HappensBeforeAnalysis::sameBarrier(const Instruction *inst1,
                                        const Instruction *inst2) const {
  ThreadAPI *threadAPI = ThreadAPI::getThreadAPI();
  if (!inst1 || !inst2 || !threadAPI) {
    return false;
  }

  const Value *barrier1 = threadAPI->getBarrierVal(inst1);
  const Value *barrier2 = threadAPI->getBarrierVal(inst2);
  if (!barrier1 || !barrier2) {
    return false;
  }
  barrier1 = barrier1->stripPointerCasts();
  barrier2 = barrier2->stripPointerCasts();
  if (barrier1 == barrier2) {
    return true;
  }
  return m_alias_analysis && m_alias_analysis->mustAlias(barrier1, barrier2);
}

const Value *HappensBeforeAnalysis::traceSharedState(const Value *value) const {
  if (!value) {
    return nullptr;
  }

  const Value *cache_key = value->stripPointerCasts();
  auto cache_it = m_shared_state_trace_cache.find(cache_key);
  if (cache_it != m_shared_state_trace_cache.end()) {
    return cache_it->second;
  }

  std::vector<const Value *> worklist = {value};
  std::unordered_set<const Value *> visited;
  const Value *resolved = nullptr;
  bool ambiguous = false;
  size_t expansions = 0;
  constexpr size_t kTraceExpansionLimit = 4096;

  while (!worklist.empty()) {
    const Value *current = worklist.back();
    worklist.pop_back();
    if (++expansions > kTraceExpansionLimit) {
      ambiguous = true;
      break;
    }
    if (!current || !visited.insert(current).second) {
      continue;
    }

    auto mapped = m_future_shared_state.find(current);
    if (mapped != m_future_shared_state.end()) {
      const Value *candidate =
          mapped->second ? mapped->second->stripPointerCasts() : nullptr;
      if (!resolved) {
        resolved = candidate;
      } else if (candidate && resolved != candidate &&
                 !(m_alias_analysis &&
                   m_alias_analysis->mustAlias(resolved, candidate))) {
        ambiguous = true;
      }
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (stripped != current) {
      worklist.push_back(stripped);
    }

    if (isa<AllocaInst>(current) || isa<GlobalValue>(current)) {
      const Value *candidate = current->stripPointerCasts();
      if (!resolved) {
        resolved = candidate;
      } else if (resolved != candidate &&
                 !(m_alias_analysis &&
                   m_alias_analysis->mustAlias(resolved, candidate))) {
        ambiguous = true;
      }
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(current)) {
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (cb && arg->getArgNo() < cb->arg_size()) {
            worklist.push_back(cb->getArgOperand(arg->getArgNo()));
            expanded = true;
          }
        }

        for (const Function &func : m_module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
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
      }
      if (!expanded) {
        const Value *candidate = current->stripPointerCasts();
        if (!resolved) {
          resolved = candidate;
        } else if (resolved != candidate &&
                   !(m_alias_analysis &&
                     m_alias_analysis->mustAlias(resolved, candidate))) {
          ambiguous = true;
        }
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(current)) {
      worklist.push_back(load->getPointerOperand());
      for (const User *user : load->getPointerOperand()->users()) {
        if (const auto *store = dyn_cast<StoreInst>(user)) {
          if (store->getPointerOperand() == load->getPointerOperand()) {
            worklist.push_back(store->getValueOperand());
          }
        }
      }
    } else if (const auto *store = dyn_cast<StoreInst>(current)) {
      worklist.push_back(store->getValueOperand());
    } else if (const auto *phi = dyn_cast<PHINode>(current)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
    } else if (const auto *select = dyn_cast<SelectInst>(current)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
    } else if (const auto *cb = dyn_cast<CallBase>(current)) {
      if (cb->arg_size() >= 1) {
        worklist.push_back(cb->getArgOperand(0));
      }
    } else if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
    }
  }

  if (ambiguous) {
    resolved = nullptr;
  }

  m_shared_state_trace_cache[cache_key] = resolved;
  return resolved;
}

bool HappensBeforeAnalysis::happensBefore(const Instruction *A,
                                          const Instruction *B) const {
  if (!A || !B) {
    return false;
  }
  if (A == B) {
    return false;
  }
  auto key = std::make_pair(A, B);
  auto cache_it = m_hb_cache.find(key);
  if (cache_it != m_hb_cache.end()) {
    return cache_it->second;
  }

  if (m_explicit_hb_pairs.count(key) != 0) {
    m_hb_cache[key] = true;
    return true;
  }
  if (canReachExplicitHB(A, B)) {
    m_hb_cache[key] = true;
    return true;
  }

  mhp::ThreadID a_tid = m_mhp.getThreadID(A);
  mhp::ThreadID b_tid = m_mhp.getThreadID(B);
  const mhp::ThreadFlowGraph &tfg = m_mhp.getThreadFlowGraph();
  std::vector<mhp::SyncNode *> start_nodes =
      a_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(A)
          : tfg.getNodes(A, a_tid);
  std::vector<mhp::SyncNode *> end_nodes =
      b_tid == std::numeric_limits<mhp::ThreadID>::max()
          ? tfg.getNodes(B)
          : tfg.getNodes(B, b_tid);
  if (start_nodes.empty() || end_nodes.empty()) {
    m_hb_cache[key] = false;
    return false;
  }

  for (mhp::SyncNode *start : start_nodes) {
    bool reached = false;
    for (mhp::SyncNode *end : end_nodes) {
      if (canReachWithHB(start, end)) {
        reached = true;
        break;
      }
    }
    if (!reached) {
      m_hb_cache[key] = false;
      return false;
    }
  }

  m_hb_cache[key] = true;
  return true;
}

} // namespace lotus

/**
 * @file JoinTargetAnalysis.cpp
 * @brief Join-target set implementation
 */

#include "Analysis/Concurrency/JoinTarget/JoinTargetAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/Utils/ThreadMultiplicity.h"

#include <algorithm>
#include <deque>
#include <set>
#include <unordered_set>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace lotus;

namespace mhp {

namespace {

bool collectUniqueStoredValues(const Value *ptr,
                               SmallVectorImpl<const Value *> &storedValues) {
  if (!ptr) {
    return false;
  }

  std::set<const Value *> uniqueValues;
  for (const User *user : ptr->users()) {
    const auto *store = dyn_cast<StoreInst>(user);
    if (!store || store->getPointerOperand() != ptr) {
      continue;
    }
    uniqueValues.insert(store->getValueOperand());
    if (uniqueValues.size() > 1) {
      storedValues.clear();
      return false;
    }
  }

  for (const Value *value : uniqueValues) {
    storedValues.push_back(value);
  }
  return !storedValues.empty();
}

} // namespace

const Value *JoinTargetAnalysis::traceThreadHandleRoot(const Value *value,
                                                       const Module *module) {
  if (!value) {
    return nullptr;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  const Value *resolved_root = nullptr;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      if (!resolved_root) {
        resolved_root = stripped;
      } else if (resolved_root != stripped) {
        return nullptr;
      }
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      SmallVector<const Value *, 2> storedValues;
      if (collectUniqueStoredValues(load->getPointerOperand(), storedValues)) {
        for (const Value *stored : storedValues) {
          worklist.push_back(stored);
        }
      } else {
        worklist.push_back(load->getPointerOperand());
      }
      continue;
    }

    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      continue;
    }

    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }

    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values()) {
        worklist.push_back(incoming);
      }
      continue;
    }

    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }

    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      if (!module) {
        return stripped;
      }
      const Function *parent = arg->getParent();
      bool expanded = false;
      if (parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (!cb || arg->getArgNo() >= cb->arg_size()) {
            continue;
          }
          worklist.push_back(cb->getArgOperand(arg->getArgNo()));
          expanded = true;
        }

        for (const Function &func : *module) {
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
        if (!resolved_root) {
          resolved_root = stripped;
        } else if (resolved_root != stripped) {
          return nullptr;
        }
      }
      continue;
    }

    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &operand : inst->operands()) {
        worklist.push_back(operand.get());
      }
      continue;
    }

    if (!resolved_root) {
      resolved_root = stripped;
    } else if (resolved_root != stripped) {
      return nullptr;
    }
  }

  return resolved_root;
}

void JoinTargetAnalysis::traceThreadHandleRoots(
    const Value *value, const Module *module,
    std::unordered_set<const Value *> &roots) {
  if (!value) return;
  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(value);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) continue;

    const Value *stripped = current->stripPointerCasts();
    if (isa<AllocaInst>(stripped) || isa<GlobalValue>(stripped)) {
      roots.insert(stripped);
      continue;
    }

    if (const auto *load = dyn_cast<LoadInst>(stripped)) {
      SmallVector<const Value *, 2> storedValues;
      if (collectUniqueStoredValues(load->getPointerOperand(), storedValues)) {
        for (const Value *stored : storedValues) {
          worklist.push_back(stored);
        }
      } else {
        worklist.push_back(load->getPointerOperand());
      }
      continue;
    }
    if (const auto *store = dyn_cast<StoreInst>(stripped)) {
      worklist.push_back(store->getPointerOperand());
      worklist.push_back(store->getValueOperand());
      continue;
    }
    if (const auto *gep = dyn_cast<GetElementPtrInst>(stripped)) {
      worklist.push_back(gep->getPointerOperand());
      continue;
    }
    if (const auto *phi = dyn_cast<PHINode>(stripped)) {
      for (const Value *incoming : phi->incoming_values())
        worklist.push_back(incoming);
      continue;
    }
    if (const auto *select = dyn_cast<SelectInst>(stripped)) {
      worklist.push_back(select->getTrueValue());
      worklist.push_back(select->getFalseValue());
      continue;
    }
    if (const auto *arg = dyn_cast<Argument>(stripped)) {
      const Function *parent = arg->getParent();
      if (module && parent) {
        for (const Use &use : parent->uses()) {
          const auto *cb = dyn_cast<CallBase>(use.getUser());
          if (!cb || arg->getArgNo() >= cb->arg_size()) continue;
          worklist.push_back(cb->getArgOperand(arg->getArgNo()));
        }
        for (const Function &func : *module) {
          for (const BasicBlock &bb : func) {
            for (const Instruction &inst : bb) {
              const auto *cb = dyn_cast<CallBase>(&inst);
              if (!cb || arg->getArgNo() >= cb->arg_size()) continue;
              const Value *called = cb->getCalledOperand();
              if (called && called->stripPointerCasts() == parent)
                worklist.push_back(cb->getArgOperand(arg->getArgNo()));
            }
          }
        }
      }
      continue;
    }
    if (const auto *inst = dyn_cast<Instruction>(stripped)) {
      for (const Use &op : inst->operands())
        worklist.push_back(op.get());
      continue;
    }
  }
}

JoinTargetAnalysis::JoinTargetAnalysis(Module &module,
                                       AliasAnalysisWrapper *aliasAnalysis)
    : m_module(module), m_threadAPI(ThreadAPI::getThreadAPI()),
      m_aliasAnalysis(aliasAnalysis) {}

void JoinTargetAnalysis::analyze() {
  collectForksAndJoins();
  m_forkToRoot.clear();
  m_joinToForks.clear();
  m_joinToFeasibleForks.clear();
  m_unambiguousJoins.clear();
  m_postDomCache.clear();

  auto mayAlias = [this](const Value *a, const Value *b) {
    if (!a || !b) return false;
    if (a->stripPointerCasts() == b->stripPointerCasts()) return true;
    if (m_aliasAnalysis) return m_aliasAnalysis->mayAlias(a, b);
    return false;
  };

  std::vector<const Instruction *> unresolvedForks;
  for (const Instruction *forkInst : m_forkInsts) {
    const Value *root =
        traceThreadHandleRoot(m_threadAPI->getForkedThread(forkInst), &m_module);
    m_forkToRoot[forkInst] = root;
    if (!root) {
      unresolvedForks.push_back(forkInst);
    }
  }

  for (const Instruction *joinInst : m_joinInsts) {
    const CallBase *joinCall = dyn_cast<CallBase>(joinInst);
    if (!joinCall || joinCall->arg_size() < 1) continue;

    std::unordered_set<const Value *> joinRoots;
    traceThreadHandleRoots(m_threadAPI->getJoinedThread(joinInst), &m_module,
                          joinRoots);

    const Value *joinArg0 = nullptr;
    if (joinRoots.empty())
      joinArg0 =
          traceThreadHandleRoot(m_threadAPI->getJoinedThread(joinInst), &m_module);

    std::vector<const Instruction *> forks;
    std::vector<const Instruction *> exact_root_matches;
    std::vector<const Instruction *> definite_matches;
    auto pushUnique = [](std::vector<const Instruction *> &out,
                         const Instruction *forkInst) {
      if (std::find(out.begin(), out.end(), forkInst) == out.end()) {
        out.push_back(forkInst);
      }
    };
    for (const Instruction *forkInst : m_forkInsts) {
      const Value *forkArg0 = nullptr;
      auto root_it = m_forkToRoot.find(forkInst);
      if (root_it != m_forkToRoot.end()) {
        forkArg0 = root_it->second;
      }
      if (!forkArg0) continue;
      bool add = false;
      if (!joinRoots.empty()) {
        for (const Value *jr : joinRoots) {
          if (jr->stripPointerCasts() == forkArg0->stripPointerCasts()) {
            pushUnique(exact_root_matches, forkInst);
            pushUnique(definite_matches, forkInst);
            add = true;
            break;
          }
          if (m_aliasAnalysis && m_aliasAnalysis->mustAlias(jr, forkArg0)) {
            pushUnique(definite_matches, forkInst);
          }
          if (mayAlias(jr, forkArg0)) {
            add = true;
            break;
          }
        }
      } else {
        if (joinArg0 && joinArg0->stripPointerCasts() == forkArg0->stripPointerCasts()) {
          pushUnique(exact_root_matches, forkInst);
          pushUnique(definite_matches, forkInst);
        } else if (joinArg0 && m_aliasAnalysis &&
                   m_aliasAnalysis->mustAlias(joinArg0, forkArg0)) {
          pushUnique(definite_matches, forkInst);
        }
        add = mayAlias(joinArg0, forkArg0);
      }
      if (add) forks.push_back(forkInst);
    }

    for (const Instruction *forkInst : unresolvedForks) {
      pushUnique(forks, forkInst);
    }

    std::vector<const Instruction *> feasible_forks =
        filterTemporallyFeasibleForks(joinInst, forks);
    std::vector<const Instruction *> feasible_exact_matches =
        filterTemporallyFeasibleForks(joinInst, exact_root_matches);
    std::vector<const Instruction *> feasible_definite_matches =
        filterTemporallyFeasibleForks(joinInst, definite_matches);

    if (classifyJoinForks(feasible_forks) == CandidateCountKind::One &&
        joinRoots.size() == 1 &&
        classifyJoinForks(feasible_exact_matches) == CandidateCountKind::One) {
      const Instruction *targetFork = feasible_exact_matches.front();
      if (m_threadMultiplicity && !m_threadMultiplicity->instructionMayExecuteMultipleTimes(targetFork)) {
        feasible_forks = std::move(feasible_exact_matches);
        m_unambiguousJoins.insert(joinInst);
      }
    } else if (classifyJoinForks(feasible_forks) == CandidateCountKind::One &&
               classifyJoinForks(feasible_definite_matches) ==
               CandidateCountKind::One) {
      const Instruction *targetFork = feasible_definite_matches.front();
      if (m_threadMultiplicity && !m_threadMultiplicity->instructionMayExecuteMultipleTimes(targetFork)) {
        feasible_forks = std::move(feasible_definite_matches);
        m_unambiguousJoins.insert(joinInst);
      }
    }

    m_joinToForks[joinInst] = std::move(forks);
    m_joinToFeasibleForks[joinInst] = std::move(feasible_forks);
  }
}

void JoinTargetAnalysis::collectForksAndJoins() {
  m_forkInsts.clear();
  m_joinInsts.clear();
  for (Function &F : m_module) {
    if (F.isDeclaration()) continue;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_threadAPI->isTDFork(inst))
        m_forkInsts.push_back(inst);
      else if (m_threadAPI->isTDJoin(inst))
        m_joinInsts.push_back(inst);
    }
  }
}

std::vector<const Instruction *>
JoinTargetAnalysis::getPossibleJoinedForks(const Instruction *joinInst) const {
  auto it = m_joinToForks.find(joinInst);
  if (it != m_joinToForks.end())
    return it->second;
  return {};
}

std::vector<const Instruction *>
JoinTargetAnalysis::getFeasibleJoinedForks(const Instruction *joinInst) const {
  auto it = m_joinToFeasibleForks.find(joinInst);
  if (it != m_joinToFeasibleForks.end()) {
    return it->second;
  }
  return {};
}

bool JoinTargetAnalysis::isUnambiguousJoin(const Instruction *joinInst) const {
  return m_unambiguousJoins.count(joinInst) != 0;
}

JoinTargetAnalysis::CandidateCountKind
JoinTargetAnalysis::classifyJoinForks(
    const std::vector<const Instruction *> &forks) const {
  if (forks.empty()) {
    return CandidateCountKind::Zero;
  }
  if (forks.size() == 1) {
    return CandidateCountKind::One;
  }
  return CandidateCountKind::Many;
}

std::vector<const Instruction *>
JoinTargetAnalysis::filterTemporallyFeasibleForks(
    const Instruction *joinInst,
    const std::vector<const Instruction *> &forks) const {
  std::vector<const Instruction *> feasible;
  if (!joinInst) {
    return feasible;
  }

  const Function *joinFunc = joinInst->getFunction();
  std::unordered_set<const Value *> joinRoots;
  traceThreadHandleRoots(m_threadAPI->getJoinedThread(joinInst), &m_module,
                         joinRoots);
  const Value *singleJoinRoot =
      joinRoots.empty()
          ? traceThreadHandleRoot(m_threadAPI->getJoinedThread(joinInst),
                                  &m_module)
          : nullptr;

  auto rootsMatch = [this](const Value *lhs, const Value *rhs) {
    if (!lhs || !rhs) {
      return false;
    }
    lhs = lhs->stripPointerCasts();
    rhs = rhs->stripPointerCasts();
    if (lhs == rhs) {
      return true;
    }
    return m_aliasAnalysis && m_aliasAnalysis->mustAlias(lhs, rhs);
  };

  auto actualMatchesJoin = [&](const Value *actual) {
    if (!actual) {
      return false;
    }
    std::unordered_set<const Value *> actualRoots;
    traceThreadHandleRoots(actual, &m_module, actualRoots);
    if (!joinRoots.empty()) {
      for (const Value *actualRoot : actualRoots) {
        for (const Value *joinRoot : joinRoots) {
          if (rootsMatch(actualRoot, joinRoot)) {
            return true;
          }
        }
      }
      return false;
    }
    return rootsMatch(traceThreadHandleRoot(actual, &m_module), singleJoinRoot);
  };

  if (!m_threadMultiplicity) {
    m_threadMultiplicity =
        std::make_unique<concurrency::ThreadMultiplicityAnalysis>(m_module);
  }

  for (const Instruction *forkInst : forks) {
    if (!forkInst || !joinInst) {
      continue;
    }
    if (forkInst->getFunction() != joinFunc) {
      const Function *helperFunc = forkInst->getFunction();
      const Value *forkHandleRoot =
          traceThreadHandleRoot(m_threadAPI->getForkedThread(forkInst));
      const auto *forkArg = dyn_cast_or_null<Argument>(
          forkHandleRoot ? forkHandleRoot->stripPointerCasts() : nullptr);
      if (!helperFunc || !forkArg || forkArg->getParent() != helperFunc) {
        continue;
      }

      std::vector<const Instruction *> matchingCallSites;
      for (const Instruction &inst : instructions(*joinFunc)) {
        const auto *cb = dyn_cast<CallBase>(&inst);
        if (!cb) {
          continue;
        }
        const Value *called = cb->getCalledOperand();
        if (!called || called->stripPointerCasts() != helperFunc) {
          continue;
        }
        if (forkArg->getArgNo() >= cb->arg_size()) {
          continue;
        }
        if (!actualMatchesJoin(cb->getArgOperand(forkArg->getArgNo()))) {
          continue;
        }
        if (!forkMayReachJoinInFunction(&inst, joinInst) ||
            joinMayReachForkInFunction(joinInst, &inst)) {
          continue;
        }
        if (m_threadMultiplicity->instructionMayExecuteMultipleTimes(&inst)) {
          matchingCallSites.clear();
          break;
        }
        matchingCallSites.push_back(&inst);
      }

      if (matchingCallSites.size() == 1) {
        feasible.push_back(forkInst);
      }
      continue;
    }
    if (!forkMayReachJoinInFunction(forkInst, joinInst)) {
      continue;
    }
    if (joinMayReachForkInFunction(joinInst, forkInst)) {
      continue;
    }
    feasible.push_back(forkInst);
  }
  return feasible;
}

bool JoinTargetAnalysis::forkMayReachJoinInFunction(
    const Instruction *forkInst, const Instruction *joinInst) const {
  if (!forkInst || !joinInst || forkInst->getFunction() != joinInst->getFunction()) {
    return false;
  }
  if (forkInst == joinInst) {
    return false;
  }

  std::deque<const BasicBlock *> worklist;
  std::unordered_set<const BasicBlock *> visited;
  const BasicBlock *start_bb = forkInst->getParent();
  const BasicBlock *goal_bb = joinInst->getParent();
  worklist.push_back(start_bb);
  visited.insert(start_bb);

  while (!worklist.empty()) {
    const BasicBlock *current = worklist.front();
    worklist.pop_front();

    if (current == goal_bb) {
      if (current != start_bb) {
        return true;
      }
      for (const Instruction &inst : *current) {
        if (&inst == forkInst) {
          continue;
        }
        if (&inst == joinInst) {
          return true;
        }
      }
      return false;
    }

    for (const BasicBlock *succ : successors(current)) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool JoinTargetAnalysis::joinMayReachForkInFunction(
    const Instruction *joinInst, const Instruction *forkInst) const {
  if (!forkInst || !joinInst || forkInst->getFunction() != joinInst->getFunction()) {
    return false;
  }
  if (joinInst == forkInst) {
    return false;
  }

  std::deque<const BasicBlock *> worklist;
  std::unordered_set<const BasicBlock *> visited;
  const BasicBlock *start_bb = joinInst->getParent();
  const BasicBlock *goal_bb = forkInst->getParent();
  worklist.push_back(start_bb);
  visited.insert(start_bb);

  while (!worklist.empty()) {
    const BasicBlock *current = worklist.front();
    worklist.pop_front();

    if (current == goal_bb) {
      if (current != start_bb) {
        return true;
      }
      bool seen_join = false;
      for (const Instruction &inst : *current) {
        if (&inst == joinInst) {
          seen_join = true;
          continue;
        }
        if (&inst == forkInst && seen_join) {
          return true;
        }
      }
      if (seen_join) {
        for (const BasicBlock *succ : successors(current)) {
          if (succ == goal_bb) {
            return true;
          }
        }
      }
    }

    for (const BasicBlock *succ : successors(current)) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

const PostDominatorTree &
JoinTargetAnalysis::getPostDominatorTree(const Function *func) const {
  auto it = m_postDomCache.find(func);
  if (it != m_postDomCache.end()) {
    return *(it->second);
  }

  auto pdt = std::make_unique<PostDominatorTree>();
  pdt->recalculate(*const_cast<Function *>(func));
  auto *pdt_ptr = pdt.get();
  m_postDomCache[func] = std::move(pdt);
  return *pdt_ptr;
}

} // namespace mhp

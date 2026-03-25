/**
 * @file NullFlowAnalysis.cpp
 * @brief Null Pointer Flow Analysis Implementation
 *
 * This file implements the NullFlowAnalysis pass which performs flow-sensitive
 * null pointer analysis using a value-flow graph and unification-based alias
 * analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup NullPointer
 */

/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Analysis/NullPointer/NullFlowAnalysis.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckValueFlowAnalysis.h"
#include "Analysis/NullPointer/API.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <limits>
#include <llvm/IR/InstIterator.h>

/**
 * @brief CLI option to limit non-null edges considered per round
 *
 * Controls the incremental analysis limit for non-null edge propagation.
 * A value of 0 means unlimited, negative values mean UINT32_MAX.
 */
static cl::opt<int> IncrementalLimits(
    "nfa-limit", cl::init(10), cl::Hidden,
    cl::desc("Determine how many non-null edges we consider a round."));

namespace lotus {
namespace nullpointer {
namespace testing {

static int IncrementalLimitOverride = std::numeric_limits<int>::min();

void setNullFlowIncrementalLimitOverrideForTesting(int Limit) {
  IncrementalLimitOverride = Limit;
}

unsigned getNullFlowIncrementalLimitForTesting() {
  if (IncrementalLimitOverride != std::numeric_limits<int>::min()) {
    return IncrementalLimitOverride <= 0 ? UINT32_MAX
                                         : static_cast<unsigned>(
                                               IncrementalLimitOverride);
  }
  return IncrementalLimits <= 0 ? UINT32_MAX
                                : static_cast<unsigned>(IncrementalLimits);
}

} // namespace testing
} // namespace nullpointer
} // namespace lotus

namespace {

bool hasGuaranteedNonNullReturn(const CallBase *CB) {
  if (!CB || !CB->getType()->isPointerTy()) {
    return false;
  }
  if (CB->hasRetAttr(Attribute::NonNull)) {
    return true;
  }
  auto *Callee = CB->getCalledFunction();
  return Callee && Callee->getAttributes().hasRetAttr(Attribute::NonNull);
}

bool isContextInsensitiveGuaranteedNonNullValue(Value *V) {
  if (!V || !V->getType()->isPointerTy()) {
    return false;
  }
  V = V->stripPointerCastsAndAliases();
  if (isa<GlobalValue>(V)) {
    return true;
  }
  if (auto *Arg = dyn_cast<Argument>(V)) {
    return Arg->hasNonNullAttr();
  }
  if (auto *I = dyn_cast<Instruction>(V)) {
    if (API::isStackAllocate(I)) {
      return true;
    }
    if (auto *CB = dyn_cast<CallBase>(I)) {
      return hasGuaranteedNonNullReturn(CB);
    }
  }
  return false;
}

} // namespace

namespace lotus {
namespace nullpointer {
namespace testing {

bool isContextInsensitiveGuaranteedNonNullValueForTesting(Value *V) {
  return ::isContextInsensitiveGuaranteedNonNullValue(V);
}

} // namespace testing
} // namespace nullpointer
} // namespace lotus

char NullFlowAnalysis::ID = 0;
static RegisterPass<NullFlowAnalysis> X("nfa", "null value flow");

/**
 * @brief Construct a new NullFlowAnalysis pass
 */
NullFlowAnalysis::NullFlowAnalysis()
    : ModulePass(ID), DAA(nullptr), VFG(nullptr) {}

/**
 * @brief Destroy the NullFlowAnalysis pass
 */
NullFlowAnalysis::~NullFlowAnalysis() = default;

/**
 * @brief Declare analysis dependencies
 * @param AU Analysis usage to register required passes
 */
void NullFlowAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<DyckValueFlowAnalysis>();
  AU.addRequired<DyckAliasAnalysis>();
}

/**
 * @brief Run the analysis on a module
 * @param M The LLVM module to analyze
 * @return true if the module was modified
 */
bool NullFlowAnalysis::runOnModule(Module &M) {
  RecursiveTimer DyckVFA("Running NFA");
  auto *VFA = &getAnalysis<DyckValueFlowAnalysis>();
  VFG = VFA->getDyckVFGraph();
  DAA = &getAnalysis<DyckAliasAnalysis>();

  // init may-null nodes
  auto MustNotNull = [this](Value *V) -> bool {
    if (isContextInsensitiveGuaranteedNonNullValue(V))
      return true;
    return !DAA->mayNull(V);
  };
  std::set<DyckVFGNode *> MayNullNodes;
  for (auto &F : M) {
    if (!F.empty())
      NewNonNullEdges[&F];
    for (auto &I : instructions(&F)) {
      if (I.getType()->isPointerTy() && !MustNotNull(&I)) {
        if (auto *INode = VFG->getVFGNode(&I)) {
          MayNullNodes.insert(INode);
        }
      }
      for (unsigned K = 0; K < I.getNumOperands(); ++K) {
        auto *Op = I.getOperand(K);
        if (Op->getType()->isPointerTy() && !MustNotNull(Op)) {
          if (auto *OpNode = VFG->getVFGNode(Op)) {
            MayNullNodes.insert(OpNode);
          }
        }
      }
    }
  }

  // dfs to get all may-null nodes (currently context-insensitive)
  std::vector<DyckVFGNode *> DFSStack(MayNullNodes.size());
  unsigned K = 0;
  for (auto *N : MayNullNodes)
    DFSStack[K++] = N;
  MayNullNodes.clear();
  std::set<DyckVFGNode *> &Visited = MayNullNodes;
  while (!DFSStack.empty()) {
    auto *Top = DFSStack.back();
    DFSStack.pop_back();
    if (Visited.count(Top))
      continue;
    Visited.insert(Top);
    for (auto &T : *Top)
      if (!Visited.count(T.first))
        DFSStack.push_back(T.first);
  }

  // get initial non null nodes
  set_intersection(Visited.begin(), Visited.end(), VFG->node_begin(),
                   VFG->node_end(),
                   inserter(NonNullNodes, NonNullNodes.begin()));
  return false;
}

/**
 * @brief Recompute null flow analysis for specific functions
 * @param NewNonNullFunctions Functions to recompute analysis for
 * @return true if any changes were made
 */
bool NullFlowAnalysis::recompute(std::set<Function *> &NewNonNullFunctions) {
  std::set<DyckVFGNode *> PossibleNonNullNodes;
  unsigned K = 0;
  unsigned Limits =
      lotus::nullpointer::testing::getNullFlowIncrementalLimitForTesting();
  for (auto &NIt : NewNonNullEdges) {
    auto EIt = NIt.second.begin();
    while (EIt != NIt.second.end()) {
      if (++K > Limits)
        break;
      auto *Src = EIt->first;
      auto *Tgt = EIt->second;
      assert(Src && Tgt);
      if (!NonNullNodes.count(Tgt))
        PossibleNonNullNodes.insert(Tgt);
      NonNullEdges.emplace(Src, Tgt);
      EIt = NIt.second.erase(EIt);
    }
  }
  if (PossibleNonNullNodes.empty())
    return false;

  unsigned OrigNonNullSize = NonNullNodes.size();
  std::vector<DyckVFGNode *> WorkList(PossibleNonNullNodes.size());
  K = 0;
  for (auto *N : PossibleNonNullNodes)
    WorkList[K++] = N;
  while (!WorkList.empty()) {
    auto *N = WorkList.back();
    WorkList.pop_back();
    if (!NonNullNodes.count(N)) {
      // check if all incoming edges of N are nonnull edges
      // if yes, N is nonnull, add N to NonNullNodes, add N's (non-null) targets
      // to WorkList if no, continue
      bool AllInNonNull = true;
      for (auto IIt = N->in_begin(), IE = N->in_end(); IIt != IE; ++IIt) {
        auto *In = IIt->first;
        if (!NonNullNodes.count(N) &&
            !NonNullEdges.count(std::make_pair(In, N))) {
          AllInNonNull = false;
          break;
        }
      }
      if (!AllInNonNull)
        continue;
      NonNullNodes.insert(N);
      if (auto *NF = N->getFunction())
        NewNonNullFunctions.insert(NF);
    }
    for (auto &T : *N)
      WorkList.push_back(T.first);
  }
  return OrigNonNullSize != NonNullNodes.size();
}

bool NullFlowAnalysis::notNull(Value *V) const {
  assert(V);
  auto *N = VFG->getVFGNode(V);
  if (!N)
    return true;
  return NonNullNodes.count(N);
}

void NullFlowAnalysis::add(Function *F, Value *V1, Value *V2) {
  auto *V1N = VFG->getVFGNode(V1);
  if (!V1N)
    return;
  auto *V2N = VFG->getVFGNode(V2);
  if (!V2N)
    return;
  NewNonNullEdges.at(F).emplace(V1N, V2N);
}

void NullFlowAnalysis::add(Function *F, CallInst *CI, unsigned int K) {
  auto *DCG = DAA->getDyckCallGraph();
  auto *DCGNode = DCG->getFunction(F);
  if (!DCGNode)
    return;
  auto *C = DCGNode->getCall(CI);
  if (!C)
    return;
  auto *Actual = CI->getArgOperand(K);
  if (auto *CC = dyn_cast<CommonCall>(C)) {
    auto *Callee = CC->getCalledFunction();
    if (K < Callee->arg_size())
      add(F, Actual, CC->getCalledFunction()->getArg(K));
    else
      assert(Callee->isVarArg());
  } else {
    auto *PC = dyn_cast<PointerCall>(C);
    for (auto *Callee : *PC)
      if (K < Callee->arg_size())
        add(F, Actual, Callee->getArg(K));
      else
        assert(Callee->isVarArg());
  }
}

void NullFlowAnalysis::add(Function *F, Value *Ret) {
  if (!Ret)
    return;
  auto *RetN = VFG->getVFGNode(Ret);
  if (!RetN)
    return;
  auto &Set = NewNonNullEdges.at(F);
  for (auto &TargetIt : *RetN)
    Set.emplace(RetN, TargetIt.first);
}

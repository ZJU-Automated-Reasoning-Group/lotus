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

#include "Analysis/NullPointer/LocalNullCheckAnalysis.h"

#include "Analysis/NullPointer/API.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Type.h>

namespace {

bool allIncomingEdgesUnreachable(Function *F, Instruction *Inst,
                                 const std::set<Edge> &UnreachableEdges) {
  if (Inst != &Inst->getParent()->front()) {
    return UnreachableEdges.count({Inst->getPrevNode(), 0});
  }

  if (Inst->getParent() == &F->getEntryBlock()) {
    return false;
  }

  bool AllPredUnreachable = true;
  for (auto PIt = pred_begin(Inst->getParent()), PE = pred_end(Inst->getParent());
       PIt != PE; ++PIt) {
    auto *PredTerm = (*PIt)->getTerminator();
    for (unsigned J = 0; J < PredTerm->getNumSuccessors(); ++J) {
      if (PredTerm->getSuccessor(J) != Inst->getParent())
        continue;
      if (!UnreachableEdges.count({PredTerm, J})) {
        AllPredUnreachable = false;
        break;
      }
    }
    if (!AllPredUnreachable)
      break;
  }
  return AllPredUnreachable;
}

} // namespace

namespace lotus {
namespace nullpointer {
namespace testing {

bool allIncomingEdgesUnreachableForTesting(Function *F, Instruction *Inst,
                                           const std::set<Edge> &Unreachable) {
  return ::allIncomingEdgesUnreachable(F, Inst, Unreachable);
}

} // namespace testing
} // namespace nullpointer
} // namespace lotus

LocalNullCheckAnalysis::LocalNullCheckAnalysis(NullFlowAnalysis *NFA,
                                               Function *F)
    : F(F), NEA(F), NFA(NFA), DT(*F) {
  // init nca
  for (unsigned K = 0; K < F->arg_size(); ++K) {
    auto *Arg = F->getArg(K);
    if (!Arg->getType()->isPointerTy())
      continue;
    auto *ArgX = NEA.get(Arg);
    if (NFA->notNull(Arg) || NFA->notNull(ArgX))
      continue;
    unsigned ID = PtrIDMap.size();
    PtrIDMap[ArgX] = ID;
  }
  for (auto &I : instructions(*F)) {
    for (unsigned K = 0; K < I.getNumOperands(); ++K) {
      auto *Op = I.getOperand(K);
      if (!Op->getType()->isPointerTy())
        continue;
      auto *OpX = NEA.get(Op);
      if (NFA->notNull(OpX) || NFA->notNull(Op))
        continue;
      auto It = PtrIDMap.find(OpX);
      if (It != PtrIDMap.end())
        continue;
      unsigned ID = PtrIDMap.size();
      PtrIDMap[NEA.get(OpX)] = ID;
    }
  }

  for (auto &B : *F)
    for (auto &I : B)
      InstNonNullMap[&I] = 0;

  label();
}

LocalNullCheckAnalysis::~LocalNullCheckAnalysis() = default;

bool LocalNullCheckAnalysis::mayNull(Value *Ptr, Instruction *Inst) {
  // not used as a ptr
  if (!Ptr->getType()->isPointerTy())
    return false;

  // must be nonnull
  if (NFA->notNull(Ptr) || NFA->notNull(NEA.get(Ptr)))
    return false;

  // ptrs in unreachable blocks are considered nonnull
  if (allIncomingEdgesUnreachable(F, Inst, UnreachableEdges))
    return false;

  // check nca results
  bool IsOperand = false;
  unsigned K = 0;
  for (; K < Inst->getNumOperands(); ++K) {
    if (Ptr == Inst->getOperand(K)) {
      IsOperand = true;
      break;
    }
  }
  assert(IsOperand && "Ptr must be an operand of Inst!");
  if (K >= 32)
    return true; // more than 32 operands in this weird instruction
  auto It = InstNonNullMap.find(Inst);
  assert(It != InstNonNullMap.end());
  return !(It->second & (1 << K));
}

void LocalNullCheckAnalysis::run() {
  // outs() << "Running on " << F->getName() << " (# Ptrs: " << PtrIDMap.size()
  // << "; # Blocks: " << F->size() << ")\n";

  // 1. init a map from each instruction to a set of nonnull pointers
  init();

  // 2. fixed-point algorithm for null check analysis
  nca();

  // 3. tag possible null pointers at each instruction
  tag();

  // 4. label unreachable edges
  label();
}

void LocalNullCheckAnalysis::init() {
  for (auto &I : instructions(*F)) {
    if (I.isTerminator()) {
      for (unsigned K = 0; K < I.getNumSuccessors(); ++K) {
        DataflowFacts[{&I, K}] = BitVector(PtrIDMap.size());
      }
    } else {
      DataflowFacts[{&I, 0}] = BitVector(PtrIDMap.size());
    }
  }
  DataflowFacts[{nullptr, 0}] = BitVector(PtrIDMap.size());
}

void LocalNullCheckAnalysis::tag() {
  BitVector ResultOfMerging;
  std::vector<Edge> IncomingEdges;
  for (auto &I : instructions(*F)) {
    auto &B = *I.getParent();
    IncomingEdges.clear();
    if (&I == &*B.begin()) {
      for (auto PIt = pred_begin(&B), PE = pred_end(&B); PIt != PE; ++PIt) {
        auto *Term = (*PIt)->getTerminator();
        for (unsigned K = 0; K < Term->getNumSuccessors(); ++K) {
          if (Term->getSuccessor(K) == I.getParent()) {
            IncomingEdges.emplace_back(Term, K);
          }
        }
      }
    } else {
      IncomingEdges.emplace_back(I.getPrevNode(), 0);
    }
    BitVector *NonNulls;
    if (IncomingEdges.empty()) {
      NonNulls = &DataflowFacts.at({nullptr, 0});
    } else if (IncomingEdges.size() == 1) {
      NonNulls = &DataflowFacts.at(IncomingEdges[0]);
    } else {
      merge(IncomingEdges, ResultOfMerging);
      NonNulls = &ResultOfMerging;
    }
    auto &Orig = InstNonNullMap[&I];
    for (auto K = 0; K < I.getNumOperands(); ++K) {
      auto *OpK = I.getOperand(K);
      auto It = PtrIDMap.find(NEA.get(OpK));
      if (It == PtrIDMap.end())
        continue;
      auto OpKMustNonNull = NonNulls->test(It->second);
      if (OpKMustNonNull) {
        Orig = Orig | (1 << K);
        if (isa<ReturnInst>(&I)) {
          NFA->add(F, OpK);
        } else if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (K < CI->arg_size())
            NFA->add(F, CI, K);
        } else {
          // ... omit others
        }
      }
    }
  }
}

void LocalNullCheckAnalysis::merge(std::vector<Edge> &IncomingEdges,
                                   BitVector &Result) {
  auto It = IncomingEdges.begin();
  Result = DataflowFacts.at(*It++);
  while (It != IncomingEdges.end()) {
    auto &NextSet = DataflowFacts.at(*It);
    Result &= NextSet;
    It++;
  }
}

void LocalNullCheckAnalysis::transfer(Edge E, const BitVector &In,
                                      BitVector &Out) {
  // assume In, evaluate E's fact as Out
  Out = In;

  auto Set = [this, &Out](Value *Ptr) {
    size_t PtrID = UINT64_MAX;
    Ptr = NEA.get(Ptr);
    auto It = PtrIDMap.find(Ptr);
    if (It != PtrIDMap.end())
      PtrID = It->second;
    if (PtrID < Out.size())
      Out.set(PtrID);
  };

  auto Count = [this, &In](Value *Ptr) -> bool {
    size_t PtrID = UINT64_MAX;
    Ptr = NEA.get(Ptr);
    auto It = PtrIDMap.find(Ptr);
    if (It != PtrIDMap.end())
      PtrID = It->second;
    if (PtrID < In.size())
      return In.test(PtrID);
    else
      return true;
  };

  // analyze each instruction type
  auto *Inst = E.first;
  auto BrNo = E.second;
  switch (Inst->getOpcode()) {
  case Instruction::Load:
  case Instruction::Store:
  case Instruction::GetElementPtr:
    Set(getPointerOperand(Inst));
    break;
  case Instruction::Alloca:
    Set(Inst);
    break;
  case Instruction::AddrSpaceCast:
  case Instruction::BitCast:
    if (Count(Inst->getOperand(0)))
      Set(Inst);
    break;
  case Instruction::PHI:
  case Instruction::Select:
    if (Inst->getType()->isPointerTy()) {
      bool AllNonNull = true;
      for (unsigned K = 0; K < Inst->getNumOperands(); ++K) {
        auto *Op = Inst->getOperand(K);
        if (!Op->getType()->isPointerTy())
          continue;
        if (!Count(Op)) {
          AllNonNull = false;
          break;
        }
      }
      if (AllNonNull)
        Set(Inst);
    }
    break;
  case Instruction::Call: {
    auto *CI = (CallInst *)Inst;
    if (auto *Callee = CI->getCalledFunction()) {
      if (Callee->isIntrinsic() &&
          Callee->getIntrinsicID() >= Intrinsic::memcpy &&
          Callee->getIntrinsicID() <=
              Intrinsic::memset_element_unordered_atomic) {
        for (unsigned K = 0; K < CI->arg_size(); ++K) {
          auto *Op = CI->getArgOperand(K);
          if (!Op->getType()->isPointerTy())
            continue;
          Set(Op);
        }
      } else if (lotus::nullpointer::testing::
                     isContextInsensitiveGuaranteedNonNullValueForTesting(CI))
        Set(Inst);
    } else {
      Set(CI->getCalledOperand());
    }
  } break;
  case Instruction::Br: {
    auto *BrInst = (BranchInst *)Inst;
    if (!BrInst->isConditional())
      break;
    auto *CmpInst = dyn_cast<ICmpInst>(BrInst->getCondition());
    if (!CmpInst)
      break;

    auto *Op0 = CmpInst->getOperand(0);
    auto *Op1 = CmpInst->getOperand(1);
    if (CmpInst->getPredicate() == CmpInst::ICMP_EQ && BrNo == 1 ||
        CmpInst->getPredicate() == CmpInst::ICMP_NE && BrNo == 0) {
      if (isa<ConstantPointerNull>(Op0))
        Set(Op1);
      else if (isa<ConstantPointerNull>(Op1))
        Set(Op0);
    }
  } break;
  default:
    break;
  }
}

void LocalNullCheckAnalysis::nca() {
  std::vector<Edge> WorkList;
  for (auto &I : instructions(*F)) {
    if (I.isTerminator()) {
      for (unsigned K = 0; K < I.getNumSuccessors(); ++K)
        WorkList.emplace_back(&I, K);
    } else {
      WorkList.emplace_back(&I, 0);
    }
  }
  std::reverse(WorkList.begin(), WorkList.end());

  std::vector<Edge> IncomingEdges;
  BitVector ResultOfMerging;
  BitVector ResultOfTransfer;
  while (!WorkList.empty()) {
    auto Edge = WorkList.back();
    WorkList.pop_back();
    if (UnreachableEdges.count(Edge))
      continue;

    auto *EdgeInst = Edge.first;
    auto &EdgeFact =
        DataflowFacts.at(Edge); // this loop iteration re-compute EdgeFact

    // 1. merge
    IncomingEdges.clear();
    if (EdgeInst == &(EdgeInst->getParent()->front())) {
      auto *B = EdgeInst->getParent();
      for (auto PIt = pred_begin(B), PE = pred_end(B); PIt != PE; ++PIt) {
        auto *Term = (*PIt)->getTerminator();
        for (unsigned K = 0; K < Term->getNumSuccessors(); ++K) {
          if (Term->getSuccessor(K) == B) {
            IncomingEdges.emplace_back(Term, K);
          }
        }
      }
    } else {
      IncomingEdges.emplace_back(EdgeInst->getPrevNode(), 0);
    }
    BitVector *NonNulls;
    if (IncomingEdges.empty()) {
      NonNulls = &DataflowFacts.at({nullptr, 0});
    } else if (IncomingEdges.size() == 1) {
      NonNulls = &DataflowFacts.at(IncomingEdges[0]);
    } else {
      merge(IncomingEdges, ResultOfMerging);
      NonNulls = &ResultOfMerging;
    }

    // 2. transfer
    ResultOfTransfer.clear();
    transfer(Edge, *NonNulls, ResultOfTransfer);

    // 3. add necessary ones to worklist
    if (ResultOfTransfer != EdgeFact) {
      EdgeFact.swap(ResultOfTransfer);
      auto *NextInst = EdgeInst->isTerminator()
                           ? &EdgeInst->getSuccessor(Edge.second)->front()
                           : EdgeInst->getNextNode();
      if (NextInst->isTerminator()) {
        for (unsigned K = 0; K < NextInst->getNumSuccessors(); ++K) {
          if (UnreachableEdges.count({NextInst, K}))
            continue;
          WorkList.emplace_back(NextInst, K);
        }
      } else if (!UnreachableEdges.count({NextInst, 0})) {
        WorkList.emplace_back(NextInst, 0);
      }
    }
  }
}

void LocalNullCheckAnalysis::label() {
  for (auto &I : instructions(*F)) {
    auto *Br = dyn_cast<BranchInst>(&I);
    if (!Br || !Br->isConditional())
      continue;

    auto *ICmp = dyn_cast<ICmpInst>(Br->getCondition());
    if (!ICmp)
      continue;
    auto Predicate = ICmp->getPredicate();
    if (Predicate != CmpInst::ICMP_EQ && Predicate != CmpInst::ICMP_NE)
      continue;
    auto *Op0 = ICmp->getOperand(0);
    auto *Op1 = ICmp->getOperand(1);

    if (Predicate == CmpInst::ICMP_EQ) {
      if (isa<ConstantPointerNull>(Op0)) {
        // null == op1
        if (!mayNull(Op1, ICmp))
          label({Br, 0});
      } else if (isa<ConstantPointerNull>(Op1)) {
        // op0 == null
        if (!mayNull(Op0, ICmp))
          label({Br, 0});
      }
    } else {
      if (isa<ConstantPointerNull>(Op0)) {
        if (!mayNull(Op1, ICmp))
          label({Br, 1});
      } else if (isa<ConstantPointerNull>(Op1)) {
        if (!mayNull(Op0, ICmp))
          label({Br, 1});
      }
    }
  }
}

void LocalNullCheckAnalysis::label(Edge E) {
  // have been labeled before, skip
  if (UnreachableEdges.count(E))
    return;
  UnreachableEdges.insert(E);

  assert(E.first->isTerminator());
  auto *Start = E.first->getParent();
  auto *End = E.first->getSuccessor(E.second);

  std::set<BasicBlock *> UnreachableBlocks;
  SmallVector<BasicBlock *, 10> Candidates;
  DT.getDescendants(End, Candidates);
  for (auto *B : Candidates) {
    if (!DT.dominates({Start, End}, B))
      continue;
    UnreachableBlocks.insert(B);
  }

  for (auto *B : UnreachableBlocks) {
    for (auto &I : *B) {
      if (I.isTerminator()) {
        for (unsigned K = 0; K < I.getNumSuccessors(); ++K) {
          if (UnreachableBlocks.count(I.getSuccessor(K))) {
            UnreachableEdges.emplace(&I, K);
          }
        }
      } else {
        UnreachableEdges.emplace(&I, 0);
      }
    }
  }
}

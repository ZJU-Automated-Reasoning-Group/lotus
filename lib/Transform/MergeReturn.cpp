//===- MergeReturn.cpp - Consolidate multiple return statements
//---------------===//
//
// This file is distributed under the MIT License. See LICENSE for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// \brief Return consolidation pass implementation
///
/// This pass consolidates multiple return statements in a function into a
/// single unified return block, simplifying control flow analysis by ensuring
/// each function has at most one exit point.
///
/// The pass creates a new unified return block and redirects all existing
/// returns to branch to this block. For non-void functions, PHI nodes are
/// created to select the appropriate return value.
///===----------------------------------------------------------------------===//

// MergeReturn pass consolidates multiple return statements in a function
// into a single unified return block. This simplifies control flow analysis
// by ensuring each function has at most one exit point.

#include "Transform/MergeReturn.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "merge-return"

// Unify multiple return blocks in a function into a single return block.
// Creates a new unified return block and redirects all existing returns to it.
// Returns true if any transformation was performed.
bool unifyReturnBlocks(Function &F) {
  std::vector<BasicBlock *> ReturningBlocks;

  for (BasicBlock &I : F) {
    if (isa<ReturnInst>(I.getTerminator()))
      ReturningBlocks.push_back(&I);

    // musttail requires call to be immediately followed by ret in the same
    // block. Merging returns would violate this invariant.
    if (auto *RI = dyn_cast<ReturnInst>(I.getTerminator())) {
      (void)RI;
      if (Instruction *Prev = I.getTerminator()->getPrevNonDebugInstruction()) {
        if (auto *CI = dyn_cast<CallInst>(Prev)) {
          if (CI->isMustTailCall())
            return false;
        }
      }
    }
  }

  if (ReturningBlocks.size() <= 1)
    return false;

  // Insert a new basic block into the function, add PHI nodes (if the function
  // returns values), and convert all the return instructions into
  // unconditional branches.
  BasicBlock *NewRetBlock =
      BasicBlock::Create(F.getContext(), "UnifiedReturnBlock", &F);

  PHINode *PN = nullptr;
  if (F.getReturnType()->isVoidTy()) {
    ReturnInst::Create(F.getContext(), nullptr, NewRetBlock);
  } else {
    // If the function doesn't return void... add a PHI node to the block...
    PN = PHINode::Create(F.getReturnType(), ReturningBlocks.size(),
                         "UnifiedRetVal");
    NewRetBlock->getInstList().push_back(PN);
    ReturnInst::Create(F.getContext(), PN, NewRetBlock);
  }

  // Loop over all of the blocks, replacing the return instruction with an
  // unconditional branch.
  for (BasicBlock *BB : ReturningBlocks) {
    // Add an incoming element to the PHI node for every return instruction that
    // is merging into this new block...
    if (PN)
      PN->addIncoming(BB->getTerminator()->getOperand(0), BB);

    BB->getInstList().pop_back(); // Remove the return insn
    BranchInst::Create(NewRetBlock, BB);
  }

  return true;
}

// New Pass Manager entry point. Applies return unification to all defined
// functions.
PreservedAnalyses MergeReturnPass::run(Module &M, ModuleAnalysisManager &) {
  bool Changed = false;
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    auto Transformed = unifyReturnBlocks(F);
    if (!Changed && Transformed)
      Changed = Transformed;
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

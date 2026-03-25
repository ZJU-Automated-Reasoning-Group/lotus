/**
 * \file ExpandAssume.cpp
 * \brief Implementation of assume expansion pass
 * \author Adapted from pagai for Lotus framework
 */

#include "Transform/ExpandAssume.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
// #include "llvm/IR/Metadata.h"

#include <vector>

namespace lotus {

char ExpandAssume::ID = 0;

// ============================================================================
// ExpandAssume Implementation
// ============================================================================

ExpandAssume::ExpandAssume() : FunctionPass(ID) { processedAssumes.clear(); }

bool ExpandAssume::runOnFunction(llvm::Function &function) {
  bool modified = false;

  // Process the function for assume expansion
  modified = processFunction(function);

  return modified;
}

void ExpandAssume::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesCFG();
}

size_t ExpandAssume::getProcessedAssumeCount() const {
  return processedAssumes.size();
}

void ExpandAssume::resetProcessedCount() { processedAssumes.clear(); }

bool ExpandAssume::isAssumeCall(llvm::Instruction *inst) {
  if (auto *callInst = llvm::dyn_cast<llvm::CallInst>(inst)) {
    if (auto *calledFunc = callInst->getCalledFunction()) {
      // Check if this is the assume intrinsic
      if (calledFunc->getName() == "llvm.assume" ||
          calledFunc->getIntrinsicID() == llvm::Intrinsic::assume) {
        return true;
      }
    }
  }
  return false;
}

llvm::Value *ExpandAssume::getAssumeCondition(llvm::CallInst *assumeCall) {
  // The first argument to assume is the condition
  if (assumeCall->getNumOperands() > 0) {
    return assumeCall->getArgOperand(0);
  }
  return nullptr;
}

llvm::Instruction *
ExpandAssume::createConditionalBranch(llvm::Value *condition,
                                      llvm::BasicBlock *originalBlock,
                                      llvm::Instruction *insertBefore) {
  llvm::LLVMContext &context = originalBlock->getContext();

  // Create the true and false basic blocks
  llvm::BasicBlock *trueBlock = llvm::BasicBlock::Create(
      context, "assume.true", originalBlock->getParent());
  llvm::BasicBlock *falseBlock = llvm::BasicBlock::Create(
      context, "assume.false", originalBlock->getParent());

  // Create the conditional branch
  llvm::IRBuilder<> builder(insertBefore);
  llvm::BranchInst *branch =
      builder.CreateCondBr(condition, trueBlock, falseBlock);

  // Move instructions after the assume call to the true block
  llvm::BasicBlock::iterator it = insertBefore->getIterator();
  llvm::BasicBlock::iterator endIt = originalBlock->end();

  // Copy instructions from after the assume call to the true block
  for (++it; it != endIt; ++it) {
    llvm::Instruction *inst = &*it;
    inst->moveBefore(*trueBlock, trueBlock->begin());
  }

  // Add terminator to true block (branch to false block for unreachable)
  llvm::IRBuilder<> trueBuilder(trueBlock);
  trueBuilder.CreateBr(falseBlock);

  // Mark false block as unreachable
  llvm::IRBuilder<> falseBuilder(falseBlock);
  falseBuilder.CreateUnreachable();

  return &*branch;
}

llvm::Instruction *ExpandAssume::splitBlockAndInsertIfThen(
    llvm::Value *condition, llvm::Instruction *splitBefore, bool unreachable) {
  llvm::BasicBlock *originalBlock = splitBefore->getParent();
  llvm::Function *function = originalBlock->getParent();

  // Create the new basic blocks
  llvm::BasicBlock *thenBlock = llvm::BasicBlock::Create(
      originalBlock->getContext(), "assume.then", function);
  llvm::BasicBlock *elseBlock = llvm::BasicBlock::Create(
      originalBlock->getContext(), "assume.else", function);

  // Split the block at the insertion point
  llvm::BasicBlock *tailBlock =
      originalBlock->splitBasicBlock(splitBefore, "assume.tail");

  // Update the terminator of the original block to branch conditionally
  llvm::Instruction *originalTerminator = originalBlock->getTerminator();
  llvm::IRBuilder<> builder(originalTerminator);

  if (unreachable) {
    // If unreachable is true, make the else block unreachable
    builder.CreateCondBr(condition, thenBlock, elseBlock);

    // Add terminator to then block
    llvm::IRBuilder<> thenBuilder(thenBlock);
    thenBuilder.CreateBr(tailBlock);

    // Mark else block as unreachable
    llvm::IRBuilder<> elseBuilder(elseBlock);
    elseBuilder.CreateUnreachable();
  } else {
    // Normal case: both branches continue to tail
    builder.CreateCondBr(condition, thenBlock, elseBlock);

    // Add terminators to both blocks
    llvm::IRBuilder<> thenBuilder(thenBlock);
    thenBuilder.CreateBr(tailBlock);

    llvm::IRBuilder<> elseBuilder(elseBlock);
    elseBuilder.CreateBr(tailBlock);
  }

  return originalTerminator;
}

bool ExpandAssume::processFunction(llvm::Function &function) {
  bool modified = false;
  std::vector<llvm::CallInst *> assumesToProcess;

  // First pass: collect all assume calls
  for (llvm::BasicBlock &block : function) {
    for (llvm::Instruction &inst : block) {
      if (isAssumeCall(&inst)) {
        if (auto *callInst = llvm::dyn_cast<llvm::CallInst>(&inst)) {
          assumesToProcess.push_back(callInst);
        }
      }
    }
  }

  // Second pass: process each assume call
  for (llvm::CallInst *assumeCall : assumesToProcess) {
    if (processedAssumes.find(assumeCall) == processedAssumes.end()) {
      if (processAssumeCall(assumeCall)) {
        modified = true;
      }
    }
  }

  return modified;
}

bool ExpandAssume::processAssumeCall(llvm::CallInst *assumeCall) {
  // Check if we've already processed this assume call
  if (processedAssumes.find(assumeCall) != processedAssumes.end()) {
    return false;
  }

  // Get the condition from the assume call
  llvm::Value *condition = getAssumeCondition(assumeCall);
  if (!condition) {
    return false;
  }

  // Get the basic block containing the assume call
  llvm::BasicBlock *block = assumeCall->getParent();

  // Find the position of the assume call in the block
  llvm::BasicBlock::iterator it = assumeCall->getIterator();
  llvm::Instruction *nextInst =
      (it != block->end()) ? &*std::next(it) : nullptr;

  // Create conditional branch based on the assume condition
  llvm::Instruction *branch =
      createConditionalBranch(condition, block, assumeCall);

  // Remove the original assume call
  assumeCall->eraseFromParent();

  // Mark this assume as processed
  processedAssumes.insert(assumeCall);

  return true;
}

} // namespace lotus

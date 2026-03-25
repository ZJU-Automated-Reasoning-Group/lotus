/**
 * @file ExpandUtils.cpp
 * @brief Helper functions for expansion passes.
 *
 * Provides utility functions used by various expansion passes to safely
 * manipulate LLVM IR, particularly handling PHI nodes and function
 * transformations.
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/ExpandUtils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace transform {

/**
 * @brief Get a safe insertion point for an instruction that replaces a use.
 *
 * If the use is in a PHI node, returns the terminator of the incoming block.
 * Otherwise, returns the instruction that contains the use. This ensures that
 * new instructions are inserted in valid locations when replacing PHI operands.
 *
 * @param use The use to find an insertion point for
 * @return Instruction pointer where new instructions can be safely inserted
 */
Instruction *phiSafeInsertPt(Use *use) {
  Instruction *insertPt = cast<Instruction>(use->getUser());
  if (PHINode *phiNode = dyn_cast<PHINode>(insertPt)) {
    insertPt = phiNode->getIncomingBlock(*use)->getTerminator();
  }
  return insertPt;
}

/**
 * @brief Replace a use with a new value, handling PHI nodes correctly.
 *
 * If the use is in a PHI node, replaces all incoming values from the same
 * incoming block. Otherwise, uses standard replaceUsesOfWith.
 *
 * @param use The use to replace
 * @param newVal The new value to use instead
 */
void phiSafeReplaceUses(Use *use, Value *newVal) {
  if (PHINode *phiNode = dyn_cast<PHINode>(use->getUser())) {
    for (unsigned i = 0; i < phiNode->getNumIncomingValues(); ++i) {
      if (phiNode->getIncomingBlock(i) == phiNode->getIncomingBlock(*use))
        phiNode->setIncomingValue(i, newVal);
    }
  } else {
    use->getUser()->replaceUsesOfWith(use->get(), newVal);
  }
}

/**
 * @brief Recreate a function with a new function type.
 *
 * Creates a new function with the given type, copies attributes and basic
 * blocks from the old function, and replaces all uses of the old function with
 * a bitcast to the new function. The old function is not deleted but becomes
 * unused.
 *
 * @param func The function to recreate
 * @param newType The new function type
 * @return Pointer to the newly created function
 */
Function *recreateFunction(Function *func, FunctionType *newType) {
  Function *newFunc = Function::Create(newType, func->getLinkage());
  newFunc->copyAttributesFrom(func);
  func->getParent()->getFunctionList().insert(
      func->getParent()->getFunctionList().begin(), newFunc);
  newFunc->takeName(func);
  newFunc->getBasicBlockList().splice(newFunc->begin(),
                                      func->getBasicBlockList());
  func->replaceAllUsesWith(ConstantExpr::getBitCast(
      newFunc, func->getFunctionType()->getPointerTo()));
  return newFunc;
}

/**
 * @brief Copy debug location information from one instruction to another.
 *
 * @param newInst The instruction to set debug info on
 * @param original The instruction to copy debug info from
 * @return newInst (for chaining)
 */
Instruction *copyDebug(Instruction *newInst, Instruction *original) {
  newInst->setDebugLoc(original->getDebugLoc());
  return newInst;
}

} // namespace transform

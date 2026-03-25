/**
 * @file ExpandConstantExpr.cpp
 * @brief Convert ConstantExprs to Instructions.
 *
 * This pass expands out ConstantExprs into Instructions. Note that this only
 * converts ConstantExprs that are referenced by Instructions. It does not
 * convert ConstantExprs that are used as initializers for global variables.
 *
 * This simplifies the IR so that later analyses don't need to handle
 * ConstantExprs when scanning through instructions.
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/ExpandConstantExpr.h"

#include "llvm/IR/Constants.h"

#include "Alias/TPA/Transforms/ExpandUtils.h"

using namespace llvm;

namespace transform {

static bool expandInstruction(Instruction *inst);

/**
 * @brief Expand a ConstantExpr into an Instruction.
 *
 * Converts the ConstantExpr to an instruction, inserts it before the given
 * insertion point, and recursively expands any ConstantExprs in the new
 * instruction's operands.
 *
 * @param insertPt Where to insert the new instruction
 * @param expr The ConstantExpr to expand
 * @return The new instruction (as a Value*)
 */
static Value *expandConstantExpr(Instruction *insertPt, ConstantExpr *expr) {
  Instruction *newInst = expr->getAsInstruction();
  newInst->insertBefore(insertPt);
  newInst->setName("expanded");
  expandInstruction(newInst);
  return newInst;
}

/**
 * @brief Expand all ConstantExpr operands in an instruction.
 *
 * Recursively expands any ConstantExpr operands of the instruction into
 * instructions. LandingPadInst is skipped as it can only accept ConstantExprs.
 *
 * @param inst The instruction to expand
 * @return true if any operands were expanded, false otherwise
 */
static bool expandInstruction(Instruction *inst) {
  // A landingpad can only accept ConstantExprs, so it should remain
  // unmodified.
  if (isa<LandingPadInst>(inst))
    return false;

  bool modified = false;
  for (unsigned opNum = 0; opNum < inst->getNumOperands(); ++opNum) {
    if (ConstantExpr *expr = dyn_cast<ConstantExpr>(inst->getOperand(opNum))) {
      modified = true;
      Use *user = &inst->getOperandUse(opNum);
      phiSafeReplaceUses(user, expandConstantExpr(phiSafeInsertPt(user), expr));
    }
  }
  return modified;
}

/**
 * @brief Run the ExpandConstantExprPass on a function.
 *
 * Scans all instructions in the function and expands any ConstantExpr operands
 * into instructions.
 *
 * @param F The function to transform
 * @param analysisManager Function analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses
ExpandConstantExprPass::run(Function &F,
                            FunctionAnalysisManager &analysisManager) {
  bool modified = false;
  for (auto &bb : F) {
    for (auto &inst : bb)
      modified |= expandInstruction(&inst);
  }
  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform

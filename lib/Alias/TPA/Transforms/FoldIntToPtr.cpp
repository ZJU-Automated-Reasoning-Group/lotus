/**
 * @file FoldIntToPtr.cpp
 * @brief Folds inttoptr instructions into simpler pointer operations.
 *
 * This pass recognizes common patterns in inttoptr instructions and replaces
 * them with more direct pointer operations:
 * - inttoptr(ptrtoint(X)) -> X (pointer copy)
 * - inttoptr(ptrtoint(X) + offset) -> gep(X, offset) (pointer arithmetic)
 *
 * This simplifies the IR and makes pointer analysis more accurate by
 * eliminating unnecessary integer conversions.
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/FoldIntToPtr.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PatternMatch.h>

using namespace llvm;
using namespace llvm::PatternMatch;

namespace transform {

/**
 * @brief Attempt to fold an inttoptr instruction into simpler operations.
 *
 * Recognizes two patterns:
 * 1. inttoptr(ptrtoint(X)) -> X (with type cast if needed)
 * 2. inttoptr(ptrtoint(X) + offset) -> gep(X, offset) (with type/offset casts
 * if needed)
 *
 * @param inst The inttoptr instruction to fold
 * @return true if the instruction was folded and removed, false otherwise
 */
static bool foldInstruction(IntToPtrInst *inst) {
  auto *op = inst->getOperand(0)->stripPointerCasts();

  // Pointer copy: Y = inttoptr (ptrtoint X)
  Value *src = nullptr;
  if (match(op, m_PtrToInt(m_Value(src)))) {
    // RAUW requires the replacement to have the exact same type.
    // `src` is a pointer, but its pointee type/address space can differ from
    // the `inttoptr` result type.
    if (src->getType() != inst->getType())
      src = CastInst::CreatePointerCast(src, inst->getType(), "src.cast", inst);
    inst->replaceAllUsesWith(src);
    inst->eraseFromParent();
    return true;
  }

  // Pointer arithmetic
  Value *offsetValue = nullptr;
  if (match(op, m_Add(m_PtrToInt(m_Value(src)), m_Value(offsetValue)))) {
    if (!offsetValue->getType()->isIntegerTy())
      return false;
    if (src->getType() != inst->getType())
      src = CastInst::CreatePointerCast(src, inst->getType(), "src.cast", inst);
    const auto &DL = inst->getModule()->getDataLayout();
    auto *indexTy = DL.getIndexType(src->getType());
    if (offsetValue->getType() != indexTy) {
      offsetValue =
          CastInst::CreateIntegerCast(offsetValue, indexTy,
                                      /*isSigned=*/true, "offset.cast", inst);
    }
    auto *gepInst =
        GetElementPtrInst::Create(inst->getType()->getPointerElementType(), src,
                                  {offsetValue}, inst->getName(), inst);
    inst->replaceAllUsesWith(gepInst);
    inst->eraseFromParent();
    return true;
  }

  return false;
}

/**
 * @brief Run the FoldIntToPtrPass on a function.
 *
 * Scans all instructions in the function for inttoptr instructions and attempts
 * to fold them into simpler pointer operations.
 *
 * @param F The function to transform
 * @param analysisManager Function analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses
FoldIntToPtrPass::run(llvm::Function &F,
                      FunctionAnalysisManager &analysisManager) {
  bool modified = false;
  for (auto &BB : F) {
    for (auto itr = BB.begin(); itr != BB.end();) {
      auto *inst = &*itr++;
      if (auto *itpInst = dyn_cast<IntToPtrInst>(inst))
        modified |= foldInstruction(itpInst);
    }
  }
  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform

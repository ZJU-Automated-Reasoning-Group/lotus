/**
 * @file ExpandGetElementPtr.cpp
 * @brief Expand complex GetElementPtr instructions into simpler ones.
 *
 * This pass breaks down complex GetElementPtr instructions into smaller ones.
 * By "complex" we mean those GEPs that perform both constant-offset pointer
 * arithmetic and variable-offset (reference an array with a variable index)
 * pointer arithmetic.
 *
 * After this pass, all GEPs in the IR are guaranteed to be in one of the
 * following forms:
 * - A constant-offset GEP, whose offset can be retrieved easily using
 *   GetPointerBaseWithConstantOffset() (e.g., y = getelementptr x, 1, 2, 3, 4)
 * - A variable-offset GEP with one index argument, which is a variable
 *   (e.g., y = getelementptr x, i)
 * - A variable-offset GEP with two index arguments, where the first index
 *   argument is 0 and the second one is a variable (e.g., y = getelementptr x,
 * 0, i)
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/ExpandGetElementPtr.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"

#include "Alias/TPA/Transforms/ExpandUtils.h"

using namespace llvm;

namespace transform {

/**
 * @brief Check if a value is a zero constant integer.
 *
 * @param V The value to check
 * @return true if V is a ConstantInt with value zero, false otherwise
 */
static bool isZeroIndex(Value *V) {
  if (auto *CI = dyn_cast<ConstantInt>(V))
    return CI->isZero();
  return false;
}

/**
 * @brief Expand a complex GEP instruction into simpler GEPs.
 *
 * If the GEP is already a simple constant-offset or variable-offset GEP,
 * returns false. Otherwise, splits it into multiple simpler GEPs.
 *
 * @param gepInst The GEP instruction to expand
 * @param dataLayout Data layout for computing offsets
 * @param ptrType Pointer type for the result
 * @return true if the GEP was expanded, false if it was already simple
 */
static bool expandGEP(GetElementPtrInst *gepInst, const DataLayout &dataLayout,
                      Type *ptrType) {
  int64_t offset = 0;
  bool isInbound = gepInst->isInBounds();
  Value *basePtr =
      GetPointerBaseWithConstantOffset(gepInst, offset, dataLayout);

  // If we can be sure that gepInst is a constant-offset GEP, just ignore it
  if (basePtr != gepInst)
    return false;

  if (gepInst->getNumOperands() <= 3)
    if (auto *cInt = dyn_cast<ConstantInt>(gepInst->getOperand(1)))
      if (cInt->isZero())
        return false;

  // errs() << "OPERATION: " << *gepInst << "\n";

  // For GEPs that has variable as an offset, simply take it out and leave all
  // other offsets alone
  std::vector<unsigned> partitions;
  for (unsigned i = 1u, e = gepInst->getNumOperands(); i < e; ++i) {
    auto *index = gepInst->getOperand(i);

    if (!isa<ConstantInt>(index))
      partitions.push_back(i);
  }

  if (partitions.empty())
    return false;
  if (partitions.size() == 1 && gepInst->getNumOperands() == 2)
    return false;

  // Phase 1: plan the split and validate that all intermediate GEPs are
  // well-typed. Never start rewriting if we can't build a valid chain.
  struct GEPStep {
    Type *sourceTy = nullptr;
    std::vector<Value *> indices;
    const char *name = nullptr;
  };

  std::vector<GEPStep> steps;
  Type *curSourceTy = gepInst->getSourceElementType();

  auto tryAddStep = [&](const std::vector<Value *> &indices,
                        const char *name) -> bool {
    if (indices.empty())
      return true;
    if (indices.size() == 1 && isZeroIndex(indices[0]))
      return true;

    Type *nextTy = GetElementPtrInst::getIndexedType(curSourceTy, indices);
    if (!nextTy)
      return false;

    steps.push_back(GEPStep{curSourceTy, indices, name});
    curSourceTy = nextTy;
    return true;
  };

  unsigned start = 0;
  unsigned lastSplit = 1;
  if (partitions[0] == 1) {
    // Keep the exact original behavior: first split can be a single variable
    // index with no leading 0.
    std::vector<Value *> indices = {gepInst->getOperand(1)};
    if (!tryAddStep(indices, "gep_array_var"))
      return false;
    start = 1;
    lastSplit = 2;
  }

  for (unsigned i = start, e = partitions.size(); i < e; ++i) {
    auto splitPoint = partitions[i];

    // Constant segment before the variable index.
    std::vector<Value *> indices;
    if (lastSplit != 1u)
      indices.push_back(ConstantInt::get(ptrType, 0));
    for (auto j = lastSplit; j < splitPoint; ++j)
      indices.push_back(gepInst->getOperand(j));
    if (!tryAddStep(indices, "gep_array_const"))
      return false;

    // Variable index segment: always {0, idx} like the original.
    indices.clear();
    indices.push_back(ConstantInt::get(ptrType, 0));
    indices.push_back(gepInst->getOperand(splitPoint));
    if (!tryAddStep(indices, "gep_array_var"))
      return false;

    lastSplit = splitPoint + 1;
  }

  if (lastSplit < gepInst->getNumOperands()) {
    std::vector<Value *> indices = {ConstantInt::get(ptrType, 0)};
    for (auto i = lastSplit; i < gepInst->getNumOperands(); ++i)
      indices.push_back(gepInst->getOperand(i));
    if (!tryAddStep(indices, "gep_array_const"))
      return false;
  }

  // Ensure the final rewritten value has the same type as the original GEP.
  if (auto *PT = dyn_cast<PointerType>(gepInst->getType())) {
    auto *expectedTy = PointerType::get(curSourceTy, PT->getAddressSpace());
    if (expectedTy != gepInst->getType())
      return false;
  } else {
    return false;
  }

  // Phase 2: rewrite using the validated plan.
  Value *ptr = gepInst->getPointerOperand();
  for (const auto &step : steps) {
    auto *newGEP = GetElementPtrInst::Create(step.sourceTy, ptr, step.indices,
                                             step.name, gepInst);
    newGEP->setIsInBounds(isInbound);
    ptr = newGEP;
  }

  ptr->takeName(gepInst);
  gepInst->replaceAllUsesWith(ptr);
  gepInst->eraseFromParent();

  return true;
}

/**
 * @brief Run the ExpandGetElementPtrPass on a function.
 *
 * Scans all GEP instructions in the function and expands complex ones into
 * simpler GEPs that are easier for pointer analysis to handle.
 *
 * @param F The function to transform
 * @param analysisManager Function analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses
ExpandGetElementPtrPass::run(llvm::Function &F,
                             FunctionAnalysisManager &analysisManager) {
  bool modified = false;
  llvm::DataLayout dataLayout(F.getParent());
  llvm::Type *ptrType = dataLayout.getIntPtrType(F.getContext());

  for (llvm::BasicBlock &BB : F) {
    for (llvm::BasicBlock::iterator itr = BB.begin(); itr != BB.end();) {
      llvm::Instruction *inst = &*itr++;
      if (llvm::GetElementPtrInst *gepInst =
              llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
        modified |= expandGEP(gepInst, dataLayout, ptrType);
      }
    }
  }
  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform

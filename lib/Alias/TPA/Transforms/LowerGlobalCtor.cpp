/**
 * @file LowerGlobalCtor.cpp
 * @brief Lowers global constructor/destructor arrays into explicit function
 * calls.
 *
 * This pass converts LLVM's @llvm.global_ctors and @llvm.global_dtors arrays
 * into explicit function calls inserted at the beginning of main(). This
 * simplifies the IR for analysis passes that don't need to handle these special
 * global arrays.
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/LowerGlobalCtor.h"

#include <map>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace transform {

/**
 * @brief Lower a global constructors array into function calls in main().
 *
 * Extracts function pointers and priorities from the @llvm.global_ctors array,
 * sorts them by priority (higher priority first), and inserts calls to them
 * at the beginning of main(). Then removes the global variable.
 *
 * @param ctor The @llvm.global_ctors global variable
 * @param module The module containing the variable
 * @return true if the transformation was applied, false if main() not found or
 * array invalid
 */
static bool lowerGlobalCtor(GlobalVariable &ctor, Module &module) {
  auto *mainFunc = module.getFunction("main");
  if (mainFunc == nullptr)
    return false;

  if (!ctor.hasUniqueInitializer())
    return false;

  auto *init = ctor.getInitializer();
  if (isa<ConstantAggregateZero>(init))
    return false;
  auto *initArray = cast<ConstantArray>(init);

  std::map<unsigned, Function *, std::greater<unsigned>> funMap;
  for (auto &elem : initArray->operands()) {
    auto *elemVal = elem.get();
    if (isa<ConstantAggregateZero>(elemVal))
      continue;
    auto *cs = cast<ConstantStruct>(elemVal);
    if (isa<ConstantPointerNull>(cs->getOperand(1)))
      continue;
    if (!isa<Function>(cs->getOperand(1)))
      continue;

    auto *prio = cast<ConstantInt>(cs->getOperand(0));
    funMap[prio->getZExtValue()] = cast<Function>(cs->getOperand(1));
  }

  auto insertPos = mainFunc->begin()->getFirstInsertionPt();
  for (auto const &mapping : funMap) {
    FunctionCallee callee(mapping.second->getFunctionType(), mapping.second);
    CallInst::Create(callee, "", &*insertPos);
  }

  ctor.eraseFromParent();
  return true;
}

/**
 * @brief Lower a global destructors array (not yet implemented).
 *
 * @param dtor The @llvm.global_dtors global variable
 * @param module The module containing the variable
 * @return false (not implemented)
 */
static bool lowerGlobalDtor(GlobalVariable &dtor, Module &module) {
  (void)dtor;
  (void)module;
  return false;
}

/**
 * @brief Run the LowerGlobalCtorPass on a module.
 *
 * Processes both @llvm.global_ctors and @llvm.global_dtors arrays if present.
 *
 * @param module The module to transform
 * @param analysisManager Module analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses
LowerGlobalCtorPass::run(Module &module,
                         ModuleAnalysisManager &analysisManager) {
  bool modified = false;

  auto *gctor = module.getGlobalVariable("llvm.global_ctors");
  if (gctor != nullptr)
    modified |= lowerGlobalCtor(*gctor, module);

  auto *gdtor = module.getGlobalVariable("llvm.global_dtors");
  if (gdtor != nullptr)
    modified |= lowerGlobalDtor(*gdtor, module);

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform

/**
 * @file GlobalCleanup.cpp
 * @brief Cleanup global symbols post-bitcode-link for TPA analysis.
 *
 * TPA input files should have no external symbols or aliases. These passes
 * internalize (or otherwise remove/resolve) GlobalValues and resolve all
 * GlobalAliases. Specifically:
 * - Removes @llvm.compiler.used and @llvm.used globals
 * - Converts ExternalWeakLinkage to null and removes the symbol
 * - Converts WeakAnyLinkage to InternalLinkage
 * - Resolves all GlobalAliases by replacing uses with their aliasee
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/GlobalCleanup.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace transform {

/**
 * @brief Clean up linkage types for a global value.
 *
 * Handles ExternalWeakLinkage (replace with null and remove) and WeakAnyLinkage
 * (convert to InternalLinkage). Other linkage types are left unchanged.
 *
 * @param gv The global value to clean up
 * @return true if the value was modified, false otherwise
 */
static bool cleanUpLinkage(GlobalValue *gv) {
  // TODO: handle the rest of the linkage types as necessary without
  // running afoul of the IR verifier or breaking the native link
  switch (gv->getLinkage()) {
  case GlobalValue::ExternalWeakLinkage: {
    Constant *nullRef = Constant::getNullValue(gv->getType());
    gv->replaceAllUsesWith(nullRef);
    gv->eraseFromParent();
    return true;
  }
  case GlobalValue::WeakAnyLinkage: {
    gv->setLinkage(GlobalValue::InternalLinkage);
    return true;
  }
  default:
    // default with fall through to avoid compiler warning
    return false;
  }
  return false;
}

/**
 * @brief Run the GlobalCleanup pass on a module.
 *
 * Removes @llvm.compiler.used and @llvm.used globals, then cleans up linkage
 * for all global variables and functions.
 *
 * @param M The module to transform
 * @param analysisManager Module analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses GlobalCleanup::run(Module &M,
                                     ModuleAnalysisManager &analysisManager) {
  bool modified = false;

  if (GlobalVariable *gv = M.getNamedGlobal("llvm.compiler.used")) {
    gv->eraseFromParent();
    modified = true;
  }
  if (GlobalVariable *gv = M.getNamedGlobal("llvm.used")) {
    gv->eraseFromParent();
    modified = true;
  }

  for (auto &gv : M.globals())
    modified |= cleanUpLinkage(&gv);

  for (auto &f : M)
    modified |= cleanUpLinkage(&f);

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

/**
 * @brief Run the ResolveAliases pass on a module.
 *
 * Replaces all uses of GlobalAliases with their aliasee values, then removes
 * the alias declarations. This simplifies the IR by eliminating alias
 * indirection.
 *
 * @param M The module to transform
 * @param analysisManager Module analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses ResolveAliases::run(Module &M,
                                      ModuleAnalysisManager &analysisManager) {
  bool modified = false;

  std::vector<GlobalAlias *> aliasToRemove;
  aliasToRemove.reserve(M.alias_size());

  for (auto &alias : M.aliases()) {
    alias.replaceAllUsesWith(alias.getAliasee());
    aliasToRemove.push_back(&alias);
    modified = true;
  }

  for (auto *alias : aliasToRemove)
    alias->eraseFromParent();
  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform

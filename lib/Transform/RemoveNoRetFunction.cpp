
// RemoveNoRetFunction pass removes function bodies that are marked as never
// returning. This simplifies analysis by eliminating functions that cannot
// return normally.

#include "Transform/RemoveNoRetFunction.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "remove-noret-function"

static llvm::cl::opt<bool> EnableDeleteNoRetBodies(
    "remove-noret-delete-bodies",
    llvm::cl::desc("Delete bodies of noreturn functions (unsafe unless"
                   " analysis-specific)"),
    llvm::cl::init(false));

// New Pass Manager entry point. Removes function bodies that are marked as
// never returning.
PreservedAnalyses RemoveNoRetFunctionPass::run(Module &M,
                                               ModuleAnalysisManager &) {
  if (!EnableDeleteNoRetBodies)
    return PreservedAnalyses::all();

  bool Changed = false;
  for (auto &F : M) {
    if (F.doesNotReturn() && !F.isDeclaration()) {
      F.deleteBody();
      F.setComdat(nullptr);
      Changed = true;
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

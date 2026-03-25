#include "Verification/Transform/ReplaceVerifierAtomic.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

namespace lotus {
namespace verification {
namespace transform {

char ReplaceVerifierAtomicPass::ID = 0;

bool ReplaceVerifierAtomicPass::runOnModule(llvm::Module &M) {
  bool changed = false;
  // nidhugg has a bug that incorrectly handles __VERIFIER_atomic_ functions
  // The only problem is in the name of the function,
  // so just rename it and use our implementations.
  auto *func = M.getFunction("__VERIFIER_atomic_begin");
  if (func) {
    func->setName("__symbiotic_atomic_begin");
    changed = true;
  }
  func = M.getFunction("__VERIFIER_atomic_end");
  if (func) {
    func->setName("__symbiotic_atomic_end");
    changed = true;
  }
  return changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::ReplaceVerifierAtomicPass>
    X("replace-verifier-atomic",
      "Replace calls to verifier atomic with calls to pthread API", false,
      false);

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createReplaceVerifierAtomicPass() {
  return new ReplaceVerifierAtomicPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus

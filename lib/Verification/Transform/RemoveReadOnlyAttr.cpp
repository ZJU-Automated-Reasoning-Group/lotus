#include "Verification/Transform/RemoveReadOnlyAttr.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

#include <set>

using namespace llvm;

namespace lotus {
namespace verification {
namespace transform {

char RemoveReadOnlyAttrPass::ID = 0;

static bool removeROAttrFromCallers(Function &F,
                                    std::set<Function *> &visitedFuns) {
  bool changed = false;
  for (auto use_it = F.use_begin(), use_end = F.use_end(); use_it != use_end;
       ++use_it) {
    CallInst *CI = dyn_cast<CallInst>(use_it->getUser());
    if (CI) {
      Function *parent = CI->getParent()->getParent();
      if (parent) {
        // Do not visit a function multiple times
        // and leave out functions from instrumentation
        if (parent->getName().startswith("__INSTR"))
          continue;

        if (!visitedFuns.insert(parent).second)
          continue;

        // Continue recursively
        if (parent->hasFnAttribute(Attribute::ReadOnly)) {
          changed = true;
          parent->removeFnAttr(Attribute::ReadOnly);
        }

        changed |= removeROAttrFromCallers(*parent, visitedFuns);
      }
    }
  }
  return changed;
}

bool RemoveReadOnlyAttrPass::runOnModule(Module &M) {
  bool changed = false;
  std::set<Function *> visitedFuns;

  for (Function &F : M) {
    if (F.getName().startswith("__INSTR")) {
      changed |= removeROAttrFromCallers(F, visitedFuns);
    }
  }

  return changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::RemoveReadOnlyAttrPass>
    X("remove-readonly-attr",
      "Remove read-only attribute from selected functions");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createRemoveReadOnlyAttrPass() {
  return new RemoveReadOnlyAttrPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus

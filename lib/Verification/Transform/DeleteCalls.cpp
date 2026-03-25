#include "Verification/Transform/DeleteCalls.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace llvm;

static cl::list<std::string>
    CallsToDelete("delete-call",
                  cl::desc("Specify which calls of functions to delete"),
                  cl::CommaSeparated);

namespace lotus {
namespace verification {
namespace transform {

char DeleteCallsPass::ID = 0;

bool DeleteCallsPass::runOnFunction(Function &F) {
  bool changed = false;
  std::set<std::string> callsset{CallsToDelete.begin(), CallsToDelete.end()};

  if (callsset.empty())
    return false;

  for (auto &B : F) {
    for (auto it = B.begin(), et = B.end(); it != et;) {
      auto &I = *it++;
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;

      auto *op = CI->getCalledOperand()->stripPointerCasts();

      auto *fun = dyn_cast<Function>(op);
      if (!fun)
        continue;

      if (callsset.find(fun->getName().str()) != callsset.end()) {
        // remove the instruction
        I.replaceAllUsesWith(UndefValue::get(I.getType()));
        I.eraseFromParent();
        changed = true;
      }
    }
  }

  return changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static RegisterPass<lotus::verification::transform::DeleteCallsPass>
    X("delete-calls", "Delete (direct) calls of the given function", false,
      false);

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createDeleteCallsPass() { return new DeleteCallsPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus

#include "Verification/Analysis/CheckModule.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::opt<std::string> detect_calls("detect-calls",
                                         cl::desc("Detect calls to functions"),
                                         cl::value_desc("function name"));

namespace lotus {
namespace verification {
namespace analysis {

char CheckModulePass::ID = 0;

bool CheckModulePass::runOnModule(Module &M) {
  if (detect_calls.empty())
    return false;

  if (!M.getFunction(detect_calls)) {
    // The function is not even declared in the module, so it cannot be called
    return false;
  }

  bool has_pointer_call = false;
  bool detected_call = false;

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;

    for (auto &B : F) {
      for (auto &I : B) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          auto *calledF = CI->getCalledFunction();
          if (!calledF) {
            has_pointer_call = true;
          } else {
            if (calledF->getName().str() == detect_calls) {
              detected_call = true;
            }
          }
        }
      }
    }
  }

  if (detected_call) {
    errs() << "Found call to function " << detect_calls << "\n";
  }
  if (has_pointer_call) {
    // This means that the detect_calls _may_ be called
    errs() << "Found a call via pointer\n";
  }

  return false;
}

} // namespace analysis
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::analysis::CheckModulePass>
    X("check-module", "Check whether the module contains given features (e.g., "
                      "calls to pthread funs)");

namespace lotus {
namespace verification {
namespace analysis {

llvm::Pass *createCheckModulePass() { return new CheckModulePass(); }

} // namespace analysis
} // namespace verification
} // namespace lotus

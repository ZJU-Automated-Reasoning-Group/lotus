#include "Verification/Analysis/GetTestTargets.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <stack>

using namespace llvm;

namespace {

bool CloneMetadata(const Instruction *i1, Instruction *i2) {
  if (i1->hasMetadata()) {
    i2->setDebugLoc(i1->getDebugLoc());
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace analysis {

char GetTestTargetsPass::ID = 0;

bool GetTestTargetsPass::runOnModule(Module &M) {
  bool changed = false;
  std::set<BasicBlock *> visited;
  std::stack<BasicBlock *> queue;
  auto &Ctx = M.getContext();
  unsigned n = 0;

  auto *mf = M.getFunction("main");
  if (!mf)
    return false;
  queue.push(&mf->getEntryBlock());

  while (!queue.empty()) {
    auto *cur = queue.top();
    queue.pop();

    bool has_call = false;
    for (auto &I : *cur) {
      if (auto *C = dyn_cast<CallInst>(&I)) {
        if (auto *F = C->getCalledFunction()) {
          if (!F->isDeclaration()) {
            has_call = true;
            auto *entry = &F->getEntryBlock();
            if (visited.insert(entry).second)
              queue.push(entry);
          }
        }
      }
    }

    if ((succ_begin(cur) == succ_end(cur)) && !has_call) {
      // Generate slicing criterion
      std::string name = "__SYMBIOTIC_test_target" + std::to_string(n++);
      auto funC = M.getOrInsertFunction(name, Type::getVoidTy(Ctx));
      auto *fun = cast<Function>(funC.getCallee());
      auto *new_CI = CallInst::Create(fun);
      auto *point = cur->getFirstNonPHI();
      CloneMetadata(point, new_CI);
      new_CI->insertBefore(point);

      changed = true;
      outs() << name << "\n";
    } else {
      for (auto *succ : successors(cur)) {
        if (visited.insert(succ).second)
          queue.push(succ);
      }
    }
  }

  return changed;
}

} // namespace analysis
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::analysis::GetTestTargetsPass>
    X("get-test-targets", "Find targets for tests generation");

namespace lotus {
namespace verification {
namespace analysis {

llvm::Pass *createGetTestTargetsPass() { return new GetTestTargetsPass(); }

} // namespace analysis
} // namespace verification
} // namespace lotus

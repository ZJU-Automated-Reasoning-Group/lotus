#include "Verification/Analysis/CountInstr.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace analysis {

char CountInstrPass::ID = 0;

bool CountInstrPass::runOnModule(Module &M) {
  uint64_t inum = 0, bnum = 0, fnum = 0, gnum = 0;

  for (auto I = M.begin(), E = M.end(); I != E; ++I) {
    // Don't count declarations
    if (I->size() == 0)
      continue;

    ++fnum;

    for (const BasicBlock &B : *I) {
      ++bnum;
      inum += B.size();
    }
  }

  for (auto I = M.global_begin(), E = M.global_end(); I != E; ++I)
    ++gnum;

  errs() << "stats: Globals/Functions/Blocks/Instr.: " << gnum << " " << fnum
         << " " << bnum << " " << inum << "\n";

  return false;
}

} // namespace analysis
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::analysis::CountInstrPass>
    X("count-instr", "Print statistics from module");

namespace lotus {
namespace verification {
namespace analysis {

llvm::Pass *createCountInstrPass() { return new CountInstrPass(); }

} // namespace analysis
} // namespace verification
} // namespace lotus

// FiTx IR builder: one FunctionPass per LLVM function; builds framework IR
// (CFG + instructions) and stores in framework_ir_[Module]. Required by
// FrameworkPass.
#include "Checker/FiTx/Framework_IR/IRGenerator.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Checker/FiTx/Core/Utils.h"

namespace ir_generator {
IRGenerator::IRGenerator() : FunctionPass(ID) {}

void IRGenerator::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<llvm::LoopInfoWrapperPass>();
}

// Build fitx::Function for F (blocks, ordered blocks, instructions) and
// register in framework_ir_[F.getParent()] for the typestate Analyzer.
bool IRGenerator::runOnFunction(llvm::Function &F) {
  auto &loop_info = getAnalysis<llvm::LoopInfoWrapperPass>().getLoopInfo();

  analyzer.analyze(F, loop_info);
  framework_ir_[F.getParent()].insert(analyzer.FrameworkFunction());
  return false;
}

}; // namespace ir_generator

std::map<llvm::Module *, std::set<std::shared_ptr<fitx::Function>>>
    ir_generator::IRGenerator::framework_ir_ =
        std::map<llvm::Module *,
                 std::set<std::shared_ptr<fitx::Function>>>();

char ir_generator::IRGenerator::ID = 1;

static llvm::RegisterPass<ir_generator::IRGenerator>
    X("ir_generator", "Generate FrameworkIR", false /* Only looks at CFG */,
      false /* Analysis Pass */);

static void registerIRGeneratorPass(const llvm::PassManagerBuilder &,
                                    llvm::legacy::PassManagerBase &PM) {
  PM.add(new ir_generator::IRGenerator());
}

static llvm::RegisterStandardPasses
    RegisterMyPass(llvm::PassManagerBuilder::EP_EarlyAsPossible,
                   registerIRGeneratorPass);

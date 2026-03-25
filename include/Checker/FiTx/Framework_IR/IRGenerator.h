/// \file IRGenerator.h
/// \brief FiTx IR builder pass: builds framework IR (CFG + instructions) from
/// LLVM per function; runs before FrameworkPass (paper §4: FiTx pipeline).
///
/// IRGenerator::runOnFunction calls Analyzer::analyze to build
/// fitx::Function (blocks, ordered block list, call/store/load/ret).
/// framework_ir_ maps llvm::Module -> set of fitx::Function for the
/// Frontend Analyzer.
#pragma once
#include "llvm/IR/Function.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Checker/FiTx/Framework_IR/Analyzer.h"

namespace ir_generator {
/// LLVM FunctionPass that builds framework IR (fitx::Function with blocks
/// and instructions) for each LLVM function. Required by FrameworkPass.
class IRGenerator : public llvm::FunctionPass {
public:
  IRGenerator();

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;

  static char ID;
  static std::map<llvm::Module *,
                  std::set<std::shared_ptr<fitx::Function>>>
      framework_ir_;

private:
  Analyzer analyzer;
}; // end of struct
} // namespace ir_generator

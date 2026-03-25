/// \file Analyzer.hpp
/// FiTx framework IR builder: builds CFG and collects return values per block
/// for function summaries and return-code aware propagation (paper §4.3).
#pragma once
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Value.h"

#include <vector>

namespace ir_generator {
class Analyzer {
public:
  Analyzer();
  void analyze(llvm::Function &function, llvm::LoopInfo &info);
  void analyzeCallInst(llvm::Instruction *instruction);
  void analyzeDebugCallInst(llvm::CallInst *call_inst);
  void analyzeReturnInst(llvm::Instruction *return_inst);
  void analyzeStoreInst(llvm::Instruction *store_inst);
  void analyzeLoadInst(llvm::Instruction *load_inst);

  std::shared_ptr<fitx::Function> FrameworkFunction() {
    return framework_function_;
  }

  bool
  collectPossibleReturnValues(llvm::Value *v,
                              std::vector<llvm::Instruction *> &visited_inst);

private:
  std::shared_ptr<fitx::Function> framework_function_;
};

} // namespace ir_generator

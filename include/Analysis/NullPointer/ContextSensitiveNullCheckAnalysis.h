#pragma once

#include <llvm/Pass.h>

#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"

class ContextSensitiveNullCheckAnalysis : public llvm::ModulePass {
private:
  ContextSensitiveNullFlowAnalysis *NFA = nullptr;

public:
  using Context = ContextSensitiveNullFlowAnalysis::Context;

  static char ID;

  ContextSensitiveNullCheckAnalysis();
  ~ContextSensitiveNullCheckAnalysis() override;

  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  bool mayNull(llvm::Value *Ptr, llvm::Instruction *Inst);
  bool mayNull(llvm::Value *Ptr, llvm::Instruction *Inst, const Context &Ctx);

  std::string getContextString(const Context &Ctx) const;
  std::vector<Context> getReachableContexts(llvm::Instruction *Inst) const;
};

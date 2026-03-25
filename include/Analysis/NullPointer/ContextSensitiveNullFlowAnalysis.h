#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "Analysis/NullPointer/AliasAnalysisAdapter.h"
#include "Analysis/NullPointer/CallStringContext.h"

class DyckAliasAnalysis;
class NullEquivalenceAnalysis;

class ContextSensitiveNullFlowAnalysis : public llvm::ModulePass {
public:
  using Context = lotus::nullpointer::CallStringContext;

  struct ContextKey {
    llvm::Instruction *Inst = nullptr;
    Context Ctx;

    bool operator<(const ContextKey &Other) const {
      if (Inst != Other.Inst) {
        return Inst < Other.Inst;
      }
      return Ctx < Other.Ctx;
    }
  };

private:
  AliasAnalysisAdapter *AAA = nullptr;
  DyckAliasAnalysis *DAA = nullptr;
  unsigned MaxContextDepth = 0;
  bool OwnsAliasAnalysisAdapter = false;

  std::set<llvm::Function *> EntryFunctions;
  std::map<ContextKey, llvm::BitVector> InFacts;
  std::map<llvm::Instruction *, std::set<Context>> ReachableContexts;
  std::map<llvm::Function *, std::unique_ptr<NullEquivalenceAnalysis>>
      Equivalence;
  std::map<llvm::Value *, unsigned> FactIds;
  std::vector<llvm::Value *> FactValues;

public:
  static char ID;

  ContextSensitiveNullFlowAnalysis();
  ~ContextSensitiveNullFlowAnalysis() override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnModule(llvm::Module &M) override;

  bool notNull(llvm::Value *Ptr, llvm::Instruction *Inst,
               const Context &Ctx) const;

  std::vector<Context> getReachableContexts(llvm::Instruction *Inst) const;

  std::string getContextString(const Context &Ctx) const;

  bool isEntryFunction(const llvm::Function *F) const;
};

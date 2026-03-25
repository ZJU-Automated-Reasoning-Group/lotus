#pragma once
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"

#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGAliasWrapper.h"

#include <memory>

namespace pdg {
class DataDependencyGraph : public llvm::ModulePass {
public:
  static char ID;
  DataDependencyGraph() : llvm::ModulePass(ID) {};
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  llvm::StringRef getPassName() const override {
    return "Data Dependency Graph";
  }
  bool runOnModule(llvm::Module &M) override;
  void addDefUseEdges(llvm::Instruction &inst);
  void addRAWEdges(llvm::Instruction &inst);
  /// @brief No-op stub kept for API compatibility; use
  /// addAliasEdgesForFunction.
  void addAliasEdges(llvm::Instruction &inst);
  /// @brief Builds DATA_ALIAS edges for all relevant instruction pairs in F.
  ///
  /// Called once per function instead of once per instruction to reduce
  /// complexity from O(n³) to O(n²).  Skips construction entirely when the
  /// over-approximate AA wrapper is unavailable to prevent graph blowup.
  void addAliasEdgesForFunction(llvm::Function &F);
  llvm::AliasResult queryAliasUnderApproximate(llvm::Value &v1,
                                               llvm::Value &v2);
  llvm::AliasResult queryAliasOverApproximate(llvm::Value &v1, llvm::Value &v2);

private:
  llvm::MemoryDependenceResults *_mem_dep_res;
  std::unique_ptr<PDGAliasWrapper>
      _alias_wrapper_over; // For over-approximation (Andersen)
  std::unique_ptr<PDGAliasWrapper>
      _alias_wrapper_under; // For under-approximation (syntactic)
};
} // namespace pdg

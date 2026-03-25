#pragma once

#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"

#include <llvm/Pass.h>

namespace lotus {
namespace gvfg {

using llvm::AnalysisUsage;
using llvm::IntraLotusAA;
using llvm::LotusAA;
using llvm::Module;
using llvm::ModulePass;
using llvm::StringRef;

class LotusGuardedValueFlowAdapterPass : public ModulePass {
public:
  static char ID;

  LotusGuardedValueFlowAdapterPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;
  // Replaces placeholder memory edges with LotusAA-backed producers and
  // materializes call-boundary interface nodes, summary nodes, imported path
  // conditions, and back-edge metadata.
  static GuardedValueFlowNode *
  safeLink(GuardedValueFlowGraph &graph, GuardedValueFlowNode *parent,
           GuardedValueFlowNode *child, float confidence = 1.0f,
           ConditionRef condition = ConditionRef::none());
  StringRef getPassName() const override {
    return "LotusGuardedValueFlowAdapterPass";
  }

private:
  bool adaptFunction(GuardedValueFlowGraph &graph, IntraLotusAA &pta,
                     LotusAA &lotus, GuardedValueFlowGraphBuilderPass &builder);
};

ModulePass *createLotusGuardedValueFlowAdapterPass();

} // namespace gvfg
} // namespace lotus

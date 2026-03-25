/*
 * Reaching Definitions Analysis (Forward)
 *
 * Analysis writers just use std::set - framework handles container optimization
 *
 * Author: rainoftime
 */
#include "Dataflow/Mono/Analyses/Intra/IntraReachingDefinitions.h"

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

using namespace llvm;

namespace mono {

namespace {

// ============================================================================
// Simple analysis implementation - uses SetContainer, can be switched to
// BitVectorContainer
// ============================================================================

using ReachingDefsDomain = LLVMMonoAnalysisDomain<SetContainer<Instruction *>>;

class ReachingDefsProblem : public IntraMonoProblem<ReachingDefsDomain> {
public:
  explicit ReachingDefsProblem(Function *F)
      : IntraMonoProblem<ReachingDefsDomain>({F}) {}

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Forward;
  }

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    mono_container_t Out = In;

    // GEN: Add this instruction if it produces a value
    if (!Inst->getType()->isVoidTy()) {
      Out.insert(Inst);
    }

    // KILL: In SSA form, definitions never kill each other
    return Out;
  }

  mono_container_t merge(const mono_container_t &Lhs,
                         const mono_container_t &Rhs) override {
    mono_container_t Out = Lhs;
    Out.unionWith(Rhs);
    return Out;
  }

  bool equal_to(const mono_container_t &Lhs,
                const mono_container_t &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }

    // Seed the entry block's first instruction with empty set
    Seeds[&*F->getEntryBlock().begin()] = {};

    return Seeds;
  }
};

} // namespace

// ============================================================================
// Public API
// ============================================================================

std::unique_ptr<DataFlowResult> runReachingDefinitionsAnalysis(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return nullptr;
  }

  // Analysis writer just creates problem and solver - framework handles
  // optimization
  ReachingDefsProblem Problem(F);
  IntraMonoSolver<ReachingDefsDomain> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      const auto &InSet = Solver.getInResultsAt(I);
      const auto &OutSet = Solver.getOutResultsAt(I);

      // Convert SetContainer<Instruction*> to std::set<Value*> (Instruction* ->
      // Value*)
      auto &InVal = Result->IN(I);
      InVal.clear();
      for (auto *Def : InSet.getSet()) {
        InVal.insert(Def);
      }
      auto &OutVal = Result->OUT(I);
      OutVal.clear();
      for (auto *Def : OutSet.getSet()) {
        OutVal.insert(Def);
      }

      // GEN[n] = {n} if n produces a value
      if (!I->getType()->isVoidTy()) {
        Result->GEN(I).insert(I);
      }

      // KILL[n] = ∅ (SSA property)
    }
  }

  return Result;
}

std::unique_ptr<DataFlowResult>
runReachingDefinitionsAnalysisBitVector(Function *F) {
  // For now, same as regular version - framework can optimize internally
  // In the future, this could use BitVectorContainer explicitly if needed
  return runReachingDefinitionsAnalysis(F);
}

} // namespace mono

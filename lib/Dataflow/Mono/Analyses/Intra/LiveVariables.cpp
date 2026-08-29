/*
 *
 * Author: rainoftime
 */
#include "Dataflow/Mono/Analyses/Intra/LiveVariables.h"

#include "Dataflow/Mono/Domains/LiveVariablesDomain.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

using namespace llvm;

namespace mono {

namespace {

class LiveVariablesProblem : public IntraMonoProblem<LiveVariablesDomain> {
public:
  explicit LiveVariablesProblem(Function *F)
      : IntraMonoProblem<LiveVariablesDomain>({F}) {}

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Backward;
  }

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    mono_container_t Out = In;

    if (!Inst->getType()->isVoidTy()) {
      Out.erase(Inst);
    }

    for (auto &Op : Inst->operands()) {
      if (isa<Instruction>(Op) || isa<Argument>(Op)) {
        Out.insert(Op);
      }
    }

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
    if (F == nullptr) {
      return Seeds;
    }
    for (auto &BB : *F) {
      if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
        Seeds[Ret] = {};
      }
    }
    return Seeds;
  }
};

} // namespace

// SSA register liveness analysis
std::unique_ptr<DataFlowResult> runLiveVariablesAnalysis(Function *f,
                                                         const DebugConfig &DebugCfg) {
  if (f == nullptr || f->isDeclaration()) {
    return nullptr;
  }

  LiveVariablesProblem Problem(f);
  IntraMonoSolver<LiveVariablesDomain> Solver(Problem);
  Solver.setDebugConfig(DebugCfg);
  Solver.solve();

  // For a backward analysis the solver's direction is reversed:
  //   - getPredsOf(n, Backward) returns CFG successors of n
  //   - getSuccsOf(n, Backward) returns CFG predecessors of n
  //
  // Therefore the solver computes:
  //   AnalysisIn[n]  = merge(AnalysisOut[s] for CFG-successors s)
  //                  = conventional OUT[n]  (live set AFTER n)
  //   AnalysisOut[n] = normalFlow(n, AnalysisIn[n])
  //                  = conventional IN[n]   (live set BEFORE n)
  //
  // The seed is placed at return instructions with an empty set, which
  // correctly initialises AnalysisIn[ret] = {} (nothing live after return).
  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *f) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      // OUT[n] = values live AFTER  n  (backward solver's AnalysisIn)
      Result->OUT(I) = Solver.getInResultsAt(I).getSet();
      // IN[n]  = values live BEFORE n  (backward solver's AnalysisOut)
      Result->IN(I) = Solver.getOutResultsAt(I).getSet();
      for (auto &Op : I->operands()) {
        if (isa<Instruction>(Op) || isa<Argument>(Op)) {
          Result->GEN(I).insert(Op);
        }
      }
      if (!I->getType()->isVoidTy()) {
        Result->KILL(I).insert(I);
      }
    }
  }

  return Result;
}

} // namespace mono

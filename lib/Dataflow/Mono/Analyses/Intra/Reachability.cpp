#include "Dataflow/Mono/Analyses/Intra/Reachability.h"

#include "Dataflow/Mono/Domains/ReachabilityDomain.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

using namespace llvm;

namespace mono {

/**
 * Reachable Analysis - A client of the monotone dataflow framework
 *
 * This is a backward dataflow analysis that computes which instructions are
 * reachable (can be executed) forward from each program point.
 *
 * Semantics of IN[i] and OUT[i]:
 *   - OUT[i] = Set of instructions reachable AFTER executing instruction i
 *   - IN[i]  = Set of instructions reachable FROM (starting at) instruction i
 *
 * Dataflow equations:
 *   - GEN[i]  = {i} if filter(i) is true, otherwise empty
 *   - KILL[i] = {} (empty set, nothing is killed)
 *   - OUT[i]  = Union of IN[succ] for all successors succ of i
 *   - IN[i]   = GEN[i] ∪ OUT[i]
 *
 * The analysis runs backward through the CFG: information flows from successors
 * to predecessors, accumulating forward reachability information.
 *
 * Author: rainoftime
 */
std::unique_ptr<DataFlowResult>
runReachableAnalysis(Function *f,
                     const std::function<bool(Instruction *i)> &filter,
                     const DebugConfig &DebugCfg) {

  if (f == nullptr || f->isDeclaration()) {
    return nullptr;
  }

  class ReachableProblem : public IntraMonoProblem<ReachabilityDomain> {
  public:
    ReachableProblem(Function *F, std::function<bool(Instruction *)> Filter)
        : IntraMonoProblem<ReachabilityDomain>({F}), Filter(std::move(Filter)) {}

    ::dataflow::controlflow::FlowDirection direction() const override {
      return ::dataflow::controlflow::FlowDirection::Backward;
    }

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Filter(Inst)) {
        Out.insert(Inst);
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

    // For a backward analysis, seeds must be placed at exit points so that
    // the solver has starting facts to propagate backward.  Without seeds,
    // allTop() = empty set is used for all nodes and normalFlow never fires
    // because no predecessor OUT is ever non-empty — the analysis produces
    // no results.
    //
    // We seed every ReturnInst with the empty set.  normalFlow will add the
    // return instruction itself (if it passes the filter) and propagate
    // backward from there.
    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
      if (F == nullptr) {
        return Seeds;
      }
      for (auto &BB : *F) {
        if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
          Seeds[Ret] = mono_container_t{};
        }
      }
      return Seeds;
    }

  private:
    std::function<bool(Instruction *)> Filter;
  };

  ReachableProblem Problem(f, filter);
  IntraMonoSolver<ReachabilityDomain> Solver(Problem);
  Solver.setDebugConfig(DebugCfg);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *f) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      Result->OUT(I) = Solver.getInResultsAt(I).getSet();
      Result->IN(I) = Solver.getOutResultsAt(I).getSet();
      if (filter(I)) {
        Result->GEN(I).insert(I);
      }
    }
  }

  return Result;
}

std::unique_ptr<DataFlowResult> runReachableAnalysis(Function *f,
                                                     const DebugConfig &DebugCfg) {

  /*
   * Create the function that doesn't filter out instructions.
   */
  auto noFilter = [](Instruction *) -> bool { return true; };

  /*
   * Run the analysis
   */
  return runReachableAnalysis(f, noFilter, DebugCfg);
}

} // namespace mono

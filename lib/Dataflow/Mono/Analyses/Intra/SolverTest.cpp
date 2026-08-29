#include "Dataflow/Mono/Analyses/Intra/SolverTest.h"

#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Support/Result.h"

using namespace llvm;

namespace mono {
namespace {

using TestAnalysisTypes = LLVMMonoAnalysisTypes<SetContainer<Value *>>;

class IntraSolverTestProblem : public IntraMonoProblem<TestAnalysisTypes> {
public:
  explicit IntraSolverTestProblem(Function *F)
      : IntraMonoProblem<TestAnalysisTypes>(std::vector<Function *>{F}) {}

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    mono_container_t Out = In;

    // Add the instruction result if it produces a value.
    if (Inst != nullptr && !Inst->getType()->isVoidTy()) {
      Out.insert(Inst);
    }
    // Add all used operands as "facts" to exercise GEN behavior.
    for (auto &Op : Inst->operands()) {
      if (auto *V = Op.get()) {
        Out.insert(V);
      }
    }
    return Out;
  }

  mono_container_t join(const mono_container_t &Lhs,
                         const mono_container_t &Rhs) override {
    mono_container_t Out = Lhs;
    Out.unionWith(Rhs);
    return Out;
  }

  bool equal(const mono_container_t &Lhs,
                const mono_container_t &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    Function *F = this->getEntryPoints().empty()
                      ? nullptr
                      : this->getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = mono_container_t{};
    return Seeds;
  }
};

} // namespace

std::unique_ptr<DataFlowResult> runIntraMonoSolverTest(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return {};
  }

  IntraSolverTestProblem Problem(F);
  IntraMonoSolver<TestAnalysisTypes> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &I : BB) {
      Result->IN(&I) = Solver.getInResultsAt(&I).getSet();
      Result->OUT(&I) = Solver.getOutResultsAt(&I).getSet();
    }
  }
  return Result;
}

} // namespace mono

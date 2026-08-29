#include "Dataflow/Mono/Analyses/Inter/SolverTest.h"

#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/InterSolver.h"

using namespace llvm;

namespace mono {
namespace {

using TestDomain = LLVMMonoAnalysisTypes<SetContainer<Value *>>;

class InterSolverTestProblem : public InterMonoProblem<TestDomain> {
public:
  explicit InterSolverTestProblem(Function *Entry)
      : InterMonoProblem<TestDomain>(std::vector<Function *>{Entry}) {}

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    mono_container_t Out = In;
    if (Inst != nullptr && !Inst->getType()->isVoidTy()) {
      Out.insert(Inst);
    }
    for (auto &Op : Inst->operands()) {
      if (auto *V = Op.get()) {
        Out.insert(V);
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

  mono_container_t callFlow(Instruction *CallSite, Function *Callee,
                            const mono_container_t &In) override {
    mono_container_t Out;
    auto *Call = dyn_cast_or_null<CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr) {
      return Out;
    }

    // Map facts from actual args to formal args to exercise call-flow.
    auto *FormalIt = Callee->arg_begin();
    for (auto &Actual : Call->args()) {
      if (FormalIt == Callee->arg_end()) {
        break;
      }
      if (In.count(Actual.get())) {
        Out.insert(&*FormalIt);
      }
      ++FormalIt;
    }

    // Preserve globals to exercise interprocedural propagation.
    for (auto *V : In) {
      if (isa<GlobalValue>(V)) {
        Out.insert(V);
      }
    }
    return Out;
  }

  mono_container_t returnFlow(Instruction *CallSite, Function *Callee,
                              Instruction *ExitStmt, Instruction *RetSite,
                              const mono_container_t &In) override {
    (void)Callee;
    (void)RetSite;

    mono_container_t Out;
    for (auto *V : In) {
      if (isa<GlobalValue>(V)) {
        Out.insert(V);
      }
    }

    auto *Ret = dyn_cast_or_null<ReturnInst>(ExitStmt);
    if (Ret != nullptr && CallSite != nullptr) {
      if (auto *RetVal = Ret->getReturnValue()) {
        if (In.count(RetVal) && !CallSite->getType()->isVoidTy()) {
          Out.insert(CallSite);
        }
      }
    }
    return Out;
  }

  mono_container_t callToRetFlow(Instruction *CallSite, Instruction *RetSite,
                                 ArrayRef<Function *> Callees,
                                 const mono_container_t &In) override {
    (void)RetSite;
    (void)Callees;
    return In;
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

InterMonoSolverTestAnalysisResult runInterMonoSolverTest(Function *Entry) {
  InterMonoSolverTestAnalysisResult Result;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Result;
  }

  InterSolverTestProblem Problem(Entry);
  InterMonoSolver<TestDomain, kDefaultInterMonoSolverTestCallStringLength>
      Solver(Problem);
  Solver.solve();

  if (const auto *Raw = Solver.getResults()) {
    Result.Results = std::make_unique<InterMonoSolverTestResult>(*Raw);
  }
  return Result;
}

} // namespace mono

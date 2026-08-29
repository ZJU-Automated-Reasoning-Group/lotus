#include "Dataflow/APA/Solver/InterSummaryTransfer.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct FakeICF {
  bool isCallSite(int Inst) const { return Inst == 10; }

  std::vector<int> getCalleesOfCallAt(int CallSite) const {
    return CallSite == 10 ? std::vector<int>{2} : std::vector<int>{};
  }

  std::vector<int> getStartPointsOf(int Function) const {
    return Function == 2 ? std::vector<int>{20} : std::vector<int>{};
  }

  std::vector<int> getExitPointsOf(int Function) const {
    return Function == 2 ? std::vector<int>{30} : std::vector<int>{};
  }

  std::vector<int> getReturnSitesOfCallAt(int CallSite) const {
    return CallSite == 10 ? std::vector<int>{11} : std::vector<int>{};
  }
};

struct FakeDomain {
  using n_t = int;
  using fact_t = int;
  using transfer_t = int;
  using f_t = int;
  using i_t = FakeICF;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};

class FakeProblem : public elimination::InterEliminationProblem<FakeDomain> {
public:
  explicit FakeProblem(const FakeICF *ICF)
      : elimination::InterEliminationProblem<FakeDomain>({1}, ICF) {}

  fact_t normalFlow(n_t Inst, const fact_t &In) override { return In + Inst; }

  fact_t join(const fact_t &Lhs, const fact_t &Rhs) const override {
    return std::max(Lhs, Rhs);
  }

  bool equal(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t bottom() const override { return 0; }

  n_t transferSuccessor(const transfer_t &T) const override {
    return T == 10 ? 11 : 0;
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    return In + CallSite + Callee;
  }

  fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t RetSite,
                    const fact_t &In) override {
    return In + CallSite + Callee + ExitStmt + RetSite;
  }

  fact_t returnFlowWithCallerFact(n_t CallSite, f_t Callee, n_t ExitStmt,
                                  n_t RetSite, const fact_t &CalleeExit,
                                  const fact_t &CallerFact) override {
    return returnFlow(CallSite, Callee, ExitStmt, RetSite, CalleeExit) +
           CallerFact;
  }

  fact_t callToRetFlow(n_t CallSite, n_t RetSite,
                       const std::vector<f_t> &Callees,
                       const fact_t &In) override {
    return In + CallSite + RetSite + static_cast<int>(Callees.size());
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override { return {}; }

  std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const override {
    return getICFG()->getCalleesOfCallAt(CallSite);
  }
};

} // namespace

TEST(InterSummaryTransferEvaluator, AppliesForwardCallEdgeEffects) {
  FakeICF ICF;
  FakeProblem Problem(&ICF);
  using ResultTy = elimination::InterDataFlowResultT<2, int, int, int>;
  using Context = mono::CallStringCTX<int, 2>;

  ResultTy Result;
  Context CallerCtx;
  Context CalleeCtx = CallerCtx;
  CalleeCtx.push_back(10);
  Result.OUT(30, CalleeCtx) = 7;

  elimination::InterSummaryTransferEvaluator<FakeDomain, 2> Evaluator(
      Problem, ICF, Result, CallerCtx);

  // Normal edge: 3 + 10 = 13.
  // Bypass: 13 + callsite(10) + retsite(11) + one callee = 35.
  // Return contribution: 7 + callsite(10) + callee(2) + exit(30) +
  // retsite(11) + caller fact(3) = 63.
  // Merge is max, so the return contribution wins.
  EXPECT_EQ(Evaluator.applyNormalEdge(10, 3), 63);
}

TEST(InterSummaryTransferEvaluator, EvaluatesSummaryTransferExpressions) {
  FakeICF ICF;
  FakeProblem Problem(&ICF);
  using ResultTy = elimination::InterDataFlowResultT<2, int, int, int>;
  using Context = mono::CallStringCTX<int, 2>;
  using Atom = elimination::InterSummaryTransferAtom<FakeDomain>;
  using Factory = elimination::PathExprFactory<Atom>;

  ResultTy Result;
  Context Ctx;
  elimination::InterSummaryTransferEvaluator<FakeDomain, 2> Evaluator(
      Problem, ICF, Result, Ctx);

  Factory Exprs;
  auto Expr = Exprs.concat(Exprs.atom(Atom::callEntry(10, 2)),
                           Exprs.atom(Atom::returnExit(10, 2, 30, 11)));

  // callEntry: 5 + 10 + 2 = 17.
  // returnExit: 17 + 10 + 2 + 30 + 11 = 70.
  EXPECT_EQ(Evaluator.evaluateExpr(Expr, 5), 70);
}

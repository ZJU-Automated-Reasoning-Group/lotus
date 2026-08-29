#include "Dataflow/NPA/Solver/Newton/Linear/Tensor/TensorProductLift.h"
#include "Dataflow/NPA/Domains/PathTransferSummary.h"
#include "Dataflow/NPA/Domains/TransformerSummary.h"
#include "Dataflow/NPA/NPA.h"

#include <unordered_map>

#include <gtest/gtest.h>

namespace {

struct BoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }

  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &p : pairs)
    out.emplace(p.first, p.second);
  return out;
}

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
evalSystem(const std::vector<std::pair<npa::Symbol, npa::E0<D>>> &eqns,
           const std::unordered_map<npa::Symbol, npa::DomVal<D>> &env) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &eqn : eqns)
    out.emplace(eqn.first, npa::I0<D>::eval(false, env, eqn.second));
  return out;
}

} // namespace

TEST(NPA, HoleCanReferenceOtherEquationVariable) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  // x = y
  // y = 1
  //
  // This exercises:
  // - Exp0::Hole lookup against ν in I0 (Kleene/NPA evaluation)
  // - Exp1::Hole lookup against the linear-solver environment in I1 (Newton
  // step)
  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto kleeneRes = npa::KleeneSolver<D>::solve(eqns);
  auto kleeneMap = toMap<D>(kleeneRes.first);
  EXPECT_TRUE(kleeneMap.at("y"));
  EXPECT_TRUE(kleeneMap.at("x"));

  auto newtonRes = npa::NPASolver<D>::solve(eqns);
  auto newtonMap = toMap<D>(newtonRes.first);
  EXPECT_TRUE(newtonMap.at("y"));
  EXPECT_TRUE(newtonMap.at("x"));
  EXPECT_TRUE(newtonRes.second.converged);
  EXPECT_FALSE(newtonRes.second.hit_limit);
  EXPECT_FALSE(newtonRes.second.hit_outer_limit);
  EXPECT_FALSE(newtonRes.second.hit_linear_limit);
  EXPECT_FALSE(newtonRes.second.hit_fixpoint_limit);
}

TEST(NPA, SolverReportsWhenOuterIterationCapReturnsApproximation) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto capped = npa::KleeneSolver<D>::solve(eqns, false, 1);
  auto cappedMap = toMap<D>(capped.first);

  EXPECT_FALSE(cappedMap.at("x"));
  EXPECT_TRUE(cappedMap.at("y"));
  EXPECT_FALSE(capped.second.converged);
  EXPECT_TRUE(capped.second.hit_limit);
  EXPECT_TRUE(capped.second.hit_outer_limit);
  EXPECT_FALSE(capped.second.hit_linear_limit);
  EXPECT_FALSE(capped.second.hit_fixpoint_limit);
  EXPECT_EQ(capped.second.equation_count, 2);
  EXPECT_EQ(capped.second.requested_max_iters, 1);
  EXPECT_EQ(capped.second.effective_max_iters, 1);
  EXPECT_EQ(capped.second.linear_strategy, npa::LinearStrategy::SCC);
}

TEST(NPA, ExactModeNewtonReportsNoApproximationSources) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto result = npa::NPASolver<D>::solve(eqns);
  auto solved = toMap<D>(result.first);

  EXPECT_TRUE(solved.at("x"));
  EXPECT_TRUE(solved.at("y"));
  EXPECT_TRUE(result.second.converged);
  EXPECT_FALSE(result.second.hit_limit);
  EXPECT_FALSE(result.second.used_approx_equal);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_FALSE(result.second.hit_fixpoint_limit);
}

TEST(NPA, NewtonInitMatchesFOfBottomAndApproximantsAreMonotone) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  std::unordered_map<npa::Symbol, npa::DomVal<D>> bottom;
  for (const auto &eqn : eqns)
    bottom.emplace(eqn.first, D::zero());
  auto f0 = evalSystem<D>(eqns, bottom);

  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu0Map = toMap<D>(nu0);
  EXPECT_EQ(nu0Map, f0);

  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0);
  auto nu1Map = toMap<D>(nu1);
  auto fNu0 = evalSystem<D>(eqns, nu0Map);

  for (const auto &eqn : eqns) {
    const auto &sym = eqn.first;
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(nu0Map.at(sym), fNu0.at(sym)));
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(fNu0.at(sym), nu1Map.at(sym)));
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(nu0Map.at(sym), nu1Map.at(sym)));
  }
}

TEST(NPA, NewtonDominatesKleeneOnIdempotentSystem) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto k0 = npa::KleeneIter<D>::init(eqns);
  auto k1 = npa::KleeneIter<D>::run(false, eqns, k0);
  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0);

  auto k0Map = toMap<D>(k0);
  auto k1Map = toMap<D>(k1);
  auto nu0Map = toMap<D>(nu0);
  auto nu1Map = toMap<D>(nu1);

  for (const auto &eqn : eqns) {
    const auto &sym = eqn.first;
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(k0Map.at(sym), nu0Map.at(sym)));
    EXPECT_TRUE(npa::domain_leq_idempotent<D>(k1Map.at(sym), nu1Map.at(sym)));
  }
}

TEST(NPA, NewtonIdempotentUpdateMatchesSolvedLinearizedSystem) {
  using D = BoolSemiring;
  using Exp0 = npa::Exp0<D>;
  using E0 = npa::E0<D>;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("x", Exp0::hole("y"));
  eqns.emplace_back("y", Exp0::term(D::one()));

  auto nu0 = npa::NewtonIter<D>::init(eqns);
  auto nu0Map = toMap<D>(nu0);

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  for (const auto &eqn : eqns) {
    auto fNu = npa::I0<D>::eval(false, nu0Map, eqn.second);
    auto diff = npa::Diff<D>::build(nu0Map, eqn.second);
    rhs.emplace_back(eqn.first, Exp1::add(Exp1::term(fNu), diff));
  }

  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());
  auto delta = npa::solve_linear_scc_impl<D>(false, rhs, init);
  auto nu1 = npa::NewtonIter<D>::run(false, eqns, nu0);

  for (size_t i = 0; i < nu1.size(); ++i)
    EXPECT_EQ(nu1[i].second, delta[i]);
}

namespace {

struct TraceSemiring {
  using value_type = std::string;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return "0"; }
  static value_type one() { return "1"; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    return "(" + a + "+" + b + ")";
  }
  static value_type extend(const value_type &a, const value_type &b) {
    return "(" + a + "*" + b + ")";
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    return "(" + a + "-" + b + ")";
  }
};

struct BadDeltaSemiring {
  using value_type = int;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return 0; }
  static value_type one() { return 1; }

  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a + b; }
  static value_type extend(value_type a, value_type b) { return a * b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }

  // Intentionally invalid: combine(nu, subtract(f(nu), nu)) != f(nu) in
  // general.
  static value_type subtract(value_type, value_type) { return 0; }
};

struct ExactDeltaSemiring {
  using value_type = int;
  using test_type = bool;
  static constexpr bool idempotent = false;

  static value_type zero() { return 0; }
  static value_type one() { return 1; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a + b; }
  static value_type extend(value_type a, value_type b) { return a * b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a - b; }
};

struct ApproxEqualityBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static void resetApproxCounter() { ApproxCounter = 0; }
  static int getApproxCounter() { return ApproxCounter; }

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static bool approx_equal(value_type a, value_type b) {
    return a == b && ApproxCounter++ >= 3;
  }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }

private:
  static int ApproxCounter;
};

int ApproxEqualityBoolSemiring::ApproxCounter = 0;

struct LimitedLinearBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr long max_linear_steps = 0;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct LimitedFixpointBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr int max_fixpoint_iters = 0;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct ContractViolationSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type, value_type) { return false; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct UnsafeProjectedBoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
  static value_type project(value_type v) { return v; }
};

struct WriteOp {
  const void *dest = nullptr;

  bool operator<(const WriteOp &other) const { return dest < other.dest; }
  bool operator==(const WriteOp &other) const { return dest == other.dest; }
};

} // namespace

namespace npa {
template <> struct TensorSemiringTraits<BadDeltaSemiring> {
  using tensor_domain = TensorProductLift<BadDeltaSemiring>;

  static bool available() { return false; }
  static bool paper_admissible() { return false; }

  static tensor_domain::value_type
  right_constant(const BadDeltaSemiring::value_type &v) {
    return {BadDeltaSemiring::one(), v};
  }

  static tensor_domain::value_type
  left_constant(const BadDeltaSemiring::value_type &v) {
    return {v, BadDeltaSemiring::one()};
  }

  static tensor_domain::value_type
  couple(const BadDeltaSemiring::value_type &lhs,
         const BadDeltaSemiring::value_type &rhs) {
    return {lhs, rhs};
  }

  static BadDeltaSemiring::value_type
  readout(const tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};
} // namespace npa

namespace {

TEST(NPA, ConcatRepresentsTwoSidedMultiplication) {
  using D = TraceSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = "X";

  // A · x · B
  E e = Exp::concat(Exp::term("A"), "x", Exp::term("B"));
  auto v = npa::I0<D>::eval(false, nu, e);
  EXPECT_EQ(v, "(A*(X*B))");
}

TEST(NPA, BoundVariablesDoNotAliasEquationVariables) {
  using D = TraceSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = "GLOBAL";

  // mu(body, x) where body references x as a *bound* variable.
  // With our TraceSemiring, fixpoint starting at 0:
  //   cur0=0, body = (A*cur), so it stabilizes at "0" only if A is "1".
  // Use body = bound(x) so result should be the initial "0".
  E e = Exp::mu(Exp::bound("x"), "x");
  auto v = npa::I0<D>::eval(false, nu, e);
  EXPECT_EQ(v, "0");
}

TEST(NPA, InvalidNonIdempotentDeltaFailsFast) {
  using D = BadDeltaSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::ndet(Exp::hole("x"), Exp::term(1)));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::InvalidNewtonDeltaError);
}

TEST(NPA, ValidNonIdempotentResidualReconstructsFunctionValue) {
  using D = ExactDeltaSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["x"] = 2;

  E expr = Exp::ndet(Exp::hole("x"), Exp::term(3));
  auto fNu = npa::I0<D>::eval(false, nu, expr);
  auto delta = D::subtract(fNu, nu.at("x"));

  EXPECT_EQ(fNu, 5);
  EXPECT_EQ(delta, 3);
  EXPECT_NO_THROW(npa::require_valid_newton_delta<D>(fNu, nu.at("x"), delta));
}

TEST(NPA, AutomaticNIterationBoundFallsBackWhenEqualityIsApproximate) {
  using D = ApproxEqualityBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  D::resetApproxCounter();

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  testing::internal::CaptureStderr();
  auto result = npa::NPASolver<D>::solve(eqns, true);
  std::string stderrOutput = testing::internal::GetCapturedStderr();

  auto solved = toMap<D>(result.first);
  EXPECT_FALSE(result.second.converged);
  EXPECT_FALSE(result.second.hit_limit);
  EXPECT_TRUE(result.second.used_approx_equal);
  EXPECT_TRUE(result.second.used_auto_n_cap);
  EXPECT_TRUE(result.second.retried_without_auto_n_cap);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_FALSE(result.second.hit_fixpoint_limit);
  EXPECT_TRUE(solved.at("x"));
  EXPECT_GE(D::getApproxCounter(), 4);
  EXPECT_NE(stderrOutput.find("automatic n-iteration bound was insufficient"),
            std::string::npos);
}

TEST(NPA, SolverCanReportDomainContractCheckFailures) {
  using D = ContractViolationSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::term(D::one()));

  auto result =
      npa::NPASolver<D>::solve(eqns, false, 1, npa::LinearStrategy::SCC,
                                  npa::DomainContractMode::BasicChecks);

  EXPECT_TRUE(result.second.domain_contract_checks_run);
  EXPECT_TRUE(result.second.domain_contract_checks_failed);
}

TEST(NPA, LinearStepLimitMarksNewtonResultAsApproximate) {
  using D = LimitedLinearBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::hole("y"));
  eqns.emplace_back("y", Exp::term(D::one()));

  auto result = npa::NPASolver<D>::solve(eqns, false, 1,
                                            npa::LinearStrategy::SCC);

  EXPECT_FALSE(result.second.converged);
  EXPECT_TRUE(result.second.hit_limit);
  EXPECT_TRUE(result.second.hit_outer_limit);
  EXPECT_TRUE(result.second.hit_linear_limit);
  EXPECT_FALSE(result.second.hit_fixpoint_limit);
}

TEST(NPA, NewtonRejectsMuExpressions) {
  using D = BoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::mu(Exp::term(D::one()), "b"));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::UnsupportedNewtonMuError);
}

TEST(NPA, NewtonRejectsUnsafeProjectExpressions) {
  using D = UnsafeProjectedBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back("x", Exp::project(Exp::term(D::one())));

  EXPECT_THROW((void)npa::NPASolver<D>::solve(eqns),
               npa::UnsafeNewtonProjectError);
}

TEST(NPA, FixpointIterationLimitMarksMuClosureAsApproximate) {
  using D = LimitedFixpointBoolSemiring;
  using Exp = npa::Exp0<D>;
  using E = npa::E0<D>;

  std::vector<std::pair<npa::Symbol, E>> eqns;
  eqns.emplace_back(
      "x", Exp::mu(Exp::ndet(Exp::bound("b"), Exp::term(D::one())), "b"));

  auto result = npa::KleeneSolver<D>::solve(eqns, false, 2);
  auto solved = toMap<D>(result.first);

  EXPECT_TRUE(solved.at("x"));
  EXPECT_FALSE(result.second.converged);
  EXPECT_TRUE(result.second.hit_limit);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_TRUE(result.second.hit_fixpoint_limit);
}

TEST(NPA, PathTransferSummaryPreservesMayWriteAcrossCombineAndExtend) {
  using D = npa::PathTransferSummary<WriteOp>;

  static int slot_a = 0;
  static int slot_b = 0;

  auto a = D::singleton(WriteOp{&slot_a});
  auto b = D::singleton(WriteOp{&slot_b});

  auto joined = D::combine(a, b);
  EXPECT_TRUE(joined.may_write.count(&slot_a));
  EXPECT_TRUE(joined.may_write.count(&slot_b));

  auto composed = D::extend(a, b);
  EXPECT_TRUE(composed.may_write.count(&slot_a));
  EXPECT_TRUE(composed.may_write.count(&slot_b));
}

TEST(NPA, TransformerSummaryPreservesMayWriteAcrossCombineAndExtend) {
  using D = npa::TransformerSummary<WriteOp>;

  static int slot_a = 0;
  static int slot_b = 0;

  auto a = D::singleton(WriteOp{&slot_a});
  auto b = D::singleton(WriteOp{&slot_b});

  auto joined = D::combine(a, b);
  EXPECT_TRUE(joined.may_write.count(&slot_a));
  EXPECT_TRUE(joined.may_write.count(&slot_b));

  auto composed = D::extend(a, b);
  EXPECT_TRUE(composed.may_write.count(&slot_a));
  EXPECT_TRUE(composed.may_write.count(&slot_b));
}

TEST(NPA, PathTransferSummaryCondCombineRespectsBooleanGuard) {
  using D = npa::PathTransferSummary<char>;

  auto thenV = D::singleton('t');
  auto elseV = D::singleton('e');

  auto chosenThen = D::condCombine(true, thenV, elseV);
  auto chosenElse = D::condCombine(false, thenV, elseV);

  EXPECT_EQ(chosenThen.paths.size(), 1u);
  EXPECT_TRUE(chosenThen.paths.count(std::vector<char>{'t'}));
  EXPECT_FALSE(chosenThen.paths.count(std::vector<char>{'e'}));

  EXPECT_EQ(chosenElse.paths.size(), 1u);
  EXPECT_TRUE(chosenElse.paths.count(std::vector<char>{'e'}));
  EXPECT_FALSE(chosenElse.paths.count(std::vector<char>{'t'}));
}

TEST(NPA, TransformerSummaryCondCombineRespectsBooleanGuard) {
  using D = npa::TransformerSummary<char>;

  auto thenV = D::singleton('t');
  auto elseV = D::singleton('e');

  auto chosenThen = D::condCombine(true, thenV, elseV);
  auto chosenElse = D::condCombine(false, thenV, elseV);

  EXPECT_EQ(chosenThen.transformers.size(), 1u);
  EXPECT_TRUE(chosenThen.transformers.count(std::vector<char>{'t'}));
  EXPECT_FALSE(chosenThen.transformers.count(std::vector<char>{'e'}));

  EXPECT_EQ(chosenElse.transformers.size(), 1u);
  EXPECT_TRUE(chosenElse.transformers.count(std::vector<char>{'e'}));
  EXPECT_FALSE(chosenElse.transformers.count(std::vector<char>{'t'}));
}

} // namespace

#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"

#include "Dataflow/NPA/NPA.h"

#include <stdexcept>

#include <gtest/gtest.h>

namespace {

using D = npa::PredicateRelationDomain;
using TD = npa::PredicateTensorDomain;

std::vector<std::pair<std::uint64_t, std::uint64_t>>
sortedTransitions(const D::value_type &relation) {
  auto transitions = D::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

std::vector<
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
sortedTensorTransitions(const TD::value_type &relation) {
  auto transitions = TD::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

} // namespace

TEST(NPA, PredicateTensorTraitsDeclarePaperAdmissibility) {
  EXPECT_TRUE(npa::TensorSemiringTraits<D>::available());
  EXPECT_TRUE(npa::TensorSemiringTraits<D>::paper_admissible());
}

TEST(NPA, PredicateTensorTraitsValidatePaperLawsWhenConfigured) {
  D::configure(1);
  EXPECT_TRUE(npa::TensorSemiringTraits<D>::validate_paper_laws());
}

TEST(NPA, PredicateRelationMaterializeRejectsInfeasibleEnumeration) {
  D::configure(13);
  EXPECT_THROW((void)D::materialize(D::one()), std::runtime_error);
}

TEST(NPA, PredicateTensorMaterializeRejectsInfeasibleEnumeration) {
  D::configure(7);
  EXPECT_THROW((void)TD::materialize(TD::one()), std::runtime_error);
}

TEST(NPA, PredicateRelationIdentityAndAssignmentCompose) {
  D::configure(1);

  auto id = D::one();
  auto set_true = D::assignConst(0, true);
  auto assume_false = D::assume(0, false);
  auto composed = D::extend(set_true, assume_false);

  EXPECT_EQ(
      sortedTransitions(id),
      (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 0}, {1, 1}}));
  EXPECT_TRUE(sortedTransitions(composed).empty());
}

TEST(NPA, PredicateRelationTensorRegularizationMatchesWorklist) {
  D::configure(1);

  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  auto set_true = Exp::term(D::assignConst(0, true));
  auto assume_false = Exp::term(D::assume(0, false));
  auto id = Exp::term(D::one());
  E1 rhs = Exp::add(Exp::concat(set_true, "X", assume_false), id);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_TRUE(D::equal(wl[0], tp[0]));
  EXPECT_EQ(
      sortedTransitions(tp[0]),
      (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 0}, {1, 1}}));
}

TEST(NPA, PredicateTensorReadoutMatchesBaseExtend) {
  D::configure(1);

  auto lhs = D::assignConst(0, true);
  auto rhs = D::assume(0, false);

  auto coupled = TD::couple(lhs, rhs);
  auto readout = TD::readout(coupled);

  EXPECT_TRUE(D::equal(readout, D::extend(lhs, rhs)));
}

TEST(NPA, PredicateTensorConstantEmbeddingsReadBackToOriginalRelation) {
  D::configure(1);

  auto relation = D::assignConst(0, true);

  auto right = npa::TensorSemiringTraits<D>::right_constant(relation);
  auto left = npa::TensorSemiringTraits<D>::left_constant(relation);

  EXPECT_TRUE(D::equal(TD::readout(right), relation));
  EXPECT_TRUE(D::equal(TD::readout(left), relation));
}

TEST(NPA, PredicateTensorReadoutAvoidsCrossTermsAcrossAlternatives) {
  D::configure(1);

  auto set_true = D::assignConst(0, true);
  auto identity = D::one();
  auto assume_false = D::assume(0, false);

  auto coupled = TD::combine(TD::couple(identity, set_true),
                             TD::couple(assume_false, identity));
  auto readout = TD::readout(coupled);

  auto expected = D::combine(D::extend(identity, set_true),
                             D::extend(assume_false, identity));
  EXPECT_TRUE(D::equal(readout, expected));
}

TEST(NPA, PredicateRelationTransposeIsInvolutive) {
  D::configure(1);

  auto relation = D::assignConst(0, true);
  auto twice = D::transpose(D::transpose(relation));

  EXPECT_TRUE(D::equal(twice, relation));
}

TEST(NPA, PredicateRelationTransposeReversesCompositionOrder) {
  D::configure(1);

  auto lhs = D::assignConst(0, true);
  auto rhs = D::assume(0, false);

  auto composed_then_transposed = D::transpose(D::extend(lhs, rhs));
  auto transposed_then_composed =
      D::extend(D::transpose(rhs), D::transpose(lhs));

  EXPECT_TRUE(D::equal(composed_then_transposed, transposed_then_composed));
}

TEST(NPA, PredicateTensorCoupleMatchesTransposeKroneckerLayout) {
  D::configure(1);

  auto lhs = D::assignConst(0, true);
  auto rhs = D::assume(0, false);
  auto coupled = TD::couple(lhs, rhs);

  std::vector<
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
      expected;
  for (const auto &left : D::materialize(D::transpose(lhs))) {
    for (const auto &right : D::materialize(rhs)) {
      expected.emplace_back(left.first, right.first, left.second, right.second);
    }
  }
  std::sort(expected.begin(), expected.end());

  EXPECT_EQ(sortedTensorTransitions(coupled), expected);
}

TEST(NPA, PredicateTensorCoupleCompositionMatchesMatchedContexts) {
  D::configure(1);

  auto a1 = D::one();
  auto b1 = D::assignConst(0, true);
  auto a2 = D::assume(0, false);
  auto b2 = D::one();

  auto lhs = TD::extend(TD::couple(a1, b1), TD::couple(a2, b2));
  auto readout = TD::readout(lhs);
  auto expected = D::extend(D::extend(a2, a1), D::extend(b1, b2));

  EXPECT_TRUE(D::equal(readout, expected));
}

TEST(NPA, PredicateTensorComposeMatchesCoupledMatchedComposition) {
  D::configure(1);

  auto a1 = D::one();
  auto b1 = D::assignConst(0, true);
  auto a2 = D::assume(0, false);
  auto b2 = D::one();

  auto lhs = TD::extend(TD::couple(a1, b1), TD::couple(a2, b2));
  auto rhs = TD::couple(D::extend(a2, a1), D::extend(b1, b2));

  EXPECT_TRUE(TD::equal(lhs, rhs));
}

TEST(NPA, PredicateTensorReadoutDistributesOverFiniteCombine) {
  D::configure(1);

  auto first = TD::couple(D::one(), D::assignConst(0, true));
  auto second = TD::couple(D::assume(0, false), D::one());

  auto lhs = TD::readout(TD::combine(first, second));
  auto rhs = D::combine(TD::readout(first), TD::readout(second));

  EXPECT_TRUE(D::equal(lhs, rhs));
}

TEST(NPA, PredicateRelationProjectElidesLocalUpdates) {
  D::configure(2, 1);

  auto set_local_true = D::assignConst(1, true);
  auto projected = D::project(set_local_true);

  EXPECT_EQ(sortedTransitions(projected),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 1}, {2, 2}, {3, 3}}));
}

TEST(NPA, PredicateRelationMergeUsesProjectedSecondOperand) {
  D::configure(2, 1);

  auto set_global_true = D::assignConst(0, true);
  auto set_local_true = D::assignConst(1, true);
  auto merged = D::merge(set_global_true, set_local_true);

  EXPECT_TRUE(D::equal(merged, set_global_true));
}

TEST(NPA, PredicateRelationProjectDistributesOverCombine) {
  D::configure(2, 1);

  auto first = D::assignConst(0, true);
  auto second = D::assignConst(1, true);

  EXPECT_TRUE(D::equal(D::project(D::combine(first, second)),
                       D::combine(D::project(first), D::project(second))));
}

TEST(NPA, PredicateRelationProjectSupportsExplicitLocalPredicateIndices) {
  D::configure(2, std::vector<unsigned>{0});

  auto set_local_true = D::assignConst(0, true);
  auto projected = D::project(set_local_true);

  EXPECT_TRUE(D::isLocalPredicate(0));
  EXPECT_FALSE(D::isLocalPredicate(1));
  EXPECT_EQ(sortedTransitions(projected),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {1, 1}, {2, 2}, {3, 3}}));
}

TEST(NPA, PredicateRelationMergeSupportsExplicitLocalPredicateIndices) {
  D::configure(2, std::vector<unsigned>{0});

  auto set_local_true = D::assignConst(0, true);
  auto set_global_true = D::assignConst(1, true);
  auto merged = D::merge(set_global_true, set_local_true);

  EXPECT_TRUE(D::equal(merged, set_global_true));
}

TEST(NPA, PredicateRelationCondCombineRespectsBooleanGuard) {
  D::configure(1);

  auto thenV = D::assignConst(0, true);
  auto elseV = D::one();

  auto chosenThen = D::condCombine(true, thenV, elseV);
  auto chosenElse = D::condCombine(false, thenV, elseV);

  EXPECT_TRUE(D::equal(chosenThen, thenV));
  EXPECT_TRUE(D::equal(chosenElse, elseV));
}

TEST(NPA, PredicateTensorProjectTSatisfiesLemma88Laws) {
  D::configure(2, 1);

  auto a = TD::couple(D::assignConst(0, true), D::one());
  auto b = TD::couple(D::one(), D::assignConst(1, true));
  auto c = TD::couple(D::assignConst(1, true), D::assignConst(0, true));

  EXPECT_TRUE(TD::equal(TD::projectT(TD::combine(a, b)),
                        TD::combine(TD::projectT(a), TD::projectT(b))));
  EXPECT_TRUE(TD::equal(TD::projectT(TD::projectT(c)), TD::projectT(c)));

  auto lhs = TD::extend(TD::projectT(a), TD::projectT(b));
  auto rhs1 = TD::projectT(TD::extend(a, TD::projectT(b)));
  auto rhs2 = TD::projectT(TD::extend(TD::projectT(a), b));

  EXPECT_TRUE(TD::equal(lhs, rhs1));
  EXPECT_TRUE(TD::equal(lhs, rhs2));
}

TEST(NPA, PredicateTensorTraitsValidatePaperLawsWithLocalPredicates) {
  D::configure(2, 1);
  EXPECT_TRUE(npa::TensorSemiringTraits<D>::validate_paper_laws());
}

TEST(NPA, PredicateTensorRegularizationSupportsProjectedLinearEquations) {
  D::configure(2, 1);

  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  auto set_global_true = Exp::term(D::assignConst(0, true));
  auto set_local_true = Exp::term(D::assignConst(1, true));
  auto id = Exp::term(D::one());
  E1 rhs = Exp::project(
      Exp::add(Exp::concat(set_global_true, "X", set_local_true), id));

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_TRUE(D::equal(wl[0], tp[0]));
  EXPECT_EQ(sortedTransitions(tp[0]),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {0, 1}, {1, 1}, {2, 2}, {2, 3}, {3, 3}}));
}

TEST(NPA, PredicateTensorTarjanSupportsProjectedLinearEquations) {
  D::configure(2, 1);

  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  auto set_global_true = Exp::term(D::assignConst(0, true));
  auto set_local_true = Exp::term(D::assignConst(1, true));
  auto id = Exp::term(D::one());
  E1 rhs = Exp::project(
      Exp::add(Exp::concat(set_global_true, "X", set_local_true), id));

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  rhs_tensor.emplace_back("X", npa::Exp1ToTensor<D>::convert(rhs));

  std::vector<typename TD::value_type> init_tensor = {TD::zero()};
  auto tarjan =
      npa::solve_linear_tensor_tarjan_impl<TD>(false, rhs_tensor, init_tensor);

  ASSERT_TRUE(tarjan.has_value());
  ASSERT_EQ((*tarjan).size(), 1u);
  EXPECT_TRUE(
      D::equal(npa::TensorSemiringTraits<D>::readout((*tarjan)[0]), wl[0]));
}

TEST(NPA, PredicateTensorDiffSupportsProjectionNodes) {
  D::configure(2, 1);

  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;
  using TDom = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["X"] = D::one();

  E0 expr = Exp0::project(
      Exp0::ndet(Exp0::term(D::one()),
                 Exp0::concat(Exp0::term(D::assignConst(0, true)), "X",
                              Exp0::term(D::assignConst(1, true)))));
  (void)npa::I0<D>::eval(false, nu, expr);

  auto ordinary = npa::Exp1ToTensor<D>::convert(npa::Diff<D>::build(nu, expr));
  auto direct = npa::TensorDiff<D>::build(nu, expr);

  std::unordered_map<npa::Symbol, typename TDom::value_type> env;
  env["X"] = npa::lift_base_value_to_tensor<D>(D::one());

  auto ordinary_val = npa::I1<TDom>::eval(false, env, ordinary);
  auto direct_val = npa::I1<TDom>::eval(false, env, direct);

  EXPECT_TRUE(TDom::equal(ordinary_val, direct_val));
}

TEST(
    NPA,
    PredicateNewtonTensorStrategyExecutesWithoutFallbackOnProjectedRecursiveEquation) {
  D::configure(2, 1);

  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);

  auto wl = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                        npa::LinearStrategy::SCC);

  testing::internal::CaptureStderr();
  auto tp = npa::NewtonSolver<D>::solve(eqns, true, -1,
                                        npa::LinearStrategy::TensorProduct);
  std::string stderr_output = testing::internal::GetCapturedStderr();

  ASSERT_EQ(wl.first.size(), 1u);
  ASSERT_EQ(tp.first.size(), 1u);
  EXPECT_TRUE(wl.second.converged);
  EXPECT_TRUE(tp.second.converged);
  EXPECT_TRUE(D::equal(wl.first[0].second, tp.first[0].second));
  EXPECT_EQ(sortedTransitions(tp.first[0].second),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {0, 1}, {1, 1}, {2, 2}, {2, 3}, {3, 3}}));
  EXPECT_EQ(stderr_output.find("falling back"), std::string::npos);
}

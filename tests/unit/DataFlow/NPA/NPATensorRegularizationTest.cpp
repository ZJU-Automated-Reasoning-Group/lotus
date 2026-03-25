#include "Dataflow/NPA/Core/LinearSolvers.h"
#include "Dataflow/NPA/Core/TensorLinearSolve.h"
#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/NPA.h"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct BoundedLangSemiring {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = false;
  static constexpr size_t MaxLen = 3;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.insert(b.begin(), b.end());
    return out;
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type extend(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &x : a) {
      for (const auto &y : b) {
        std::string s = x + y;
        if (s.size() <= MaxLen)
          out.insert(std::move(s));
      }
    }
    return out;
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &x : a)
      if (b.find(x) == b.end())
        out.insert(x);
    return out;
  }
};

struct CustomTensorLangSemiring {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = false;
  static constexpr size_t MaxLen = 3;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.insert(b.begin(), b.end());
    return out;
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type extend(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &x : a) {
      for (const auto &y : b) {
        std::string s = x + y;
        if (s.size() <= MaxLen)
          out.insert(std::move(s));
      }
    }
    return out;
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &x : a)
      if (b.find(x) == b.end())
        out.insert(x);
    return out;
  }
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &p : pairs)
    out.emplace(p.first, p.second);
  return out;
}

static BoundedLangSemiring::value_type singleton(const std::string &s) {
  return {s};
}

template <class D>
std::vector<npa::DomVal<D>> solve_with_unchecked_tensorized_helper(
    const std::vector<std::pair<npa::Symbol, npa::E1<D>>> &eqns,
    const std::vector<npa::DomVal<D>> &init) {
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;
  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  rhs_tensor.reserve(eqns.size());
  for (const auto &eqn : eqns)
    rhs_tensor.emplace_back(eqn.first,
                            npa::Exp1ToTensor<D>::convert(eqn.second));
  return npa::solve_linear_tensorized_impl<D>(false, rhs_tensor, init);
}

} // namespace

namespace npa {
template <> struct TensorSemiringTraits<BoundedLangSemiring> {
  using tensor_domain = TensorProductExactDomain<BoundedLangSemiring>;

  static bool available() { return true; }
  static bool paper_admissible() { return false; }

  static tensor_domain::value_type
  right_constant(const BoundedLangSemiring::value_type &v) {
    return domain_equal<BoundedLangSemiring>(v, BoundedLangSemiring::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(BoundedLangSemiring::one(), v);
  }

  static tensor_domain::value_type
  left_constant(const BoundedLangSemiring::value_type &v) {
    return domain_equal<BoundedLangSemiring>(v, BoundedLangSemiring::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(v, BoundedLangSemiring::one());
  }

  static tensor_domain::value_type
  constant(const BoundedLangSemiring::value_type &v) {
    return right_constant(v);
  }

  static tensor_domain::value_type
  couple(const BoundedLangSemiring::value_type &lhs,
         const BoundedLangSemiring::value_type &rhs) {
    return tensor_domain::singleton(lhs, rhs);
  }

  static BoundedLangSemiring::value_type
  readout(const tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};

template <> struct TensorSemiringTraits<CustomTensorLangSemiring> {
  using tensor_domain = TensorProductExactDomain<CustomTensorLangSemiring>;

  static bool available() { return true; }
  static bool paper_admissible() { return false; }

  static tensor_domain::value_type
  right_constant(const CustomTensorLangSemiring::value_type &v) {
    return domain_equal<CustomTensorLangSemiring>(
               v, CustomTensorLangSemiring::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(CustomTensorLangSemiring::one(), v);
  }

  static tensor_domain::value_type
  left_constant(const CustomTensorLangSemiring::value_type &v) {
    return domain_equal<CustomTensorLangSemiring>(
               v, CustomTensorLangSemiring::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(v, CustomTensorLangSemiring::one());
  }

  static tensor_domain::value_type
  constant(const CustomTensorLangSemiring::value_type &v) {
    return right_constant(v);
  }

  static tensor_domain::value_type
  couple(const CustomTensorLangSemiring::value_type &lhs,
         const CustomTensorLangSemiring::value_type &rhs) {
    return tensor_domain::singleton(lhs, rhs);
  }

  static CustomTensorLangSemiring::value_type
  readout(const tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};
} // namespace npa

TEST(NPA, TensorRegularizationMatchesWorklistOnConstantConcat) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  // X = a · X · b  ⊕  c
  auto a = Exp::term(singleton("a"));
  auto b = Exp::term(singleton("b"));
  auto c = Exp::term(singleton("c"));
  E1 rhs = Exp::add(Exp::concat(a, "X", b), c);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);

  // Bounded by MaxLen=3: least solution includes { "c", "acb" }.
  BoundedLangSemiring::value_type expect = {"c", "acb"};
  EXPECT_EQ(wl[0], expect);
}

TEST(NPA, TensorTraitsDistinguishPaperAdmissibleAndUtilityModes) {
  EXPECT_TRUE(npa::TensorSemiringTraits<
              npa::PredicateRelationDomain>::paper_admissible());
  EXPECT_FALSE(
      npa::TensorSemiringTraits<BoundedLangSemiring>::paper_admissible());
  EXPECT_FALSE(
      npa::TensorSemiringTraits<CustomTensorLangSemiring>::paper_admissible());
}

TEST(NPA, TensorRegularizationPreservesCorrelationAcrossAlternatives) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  // X = (a · X · b) ⊕ (c · X · d) ⊕ x
  //
  // Under bounded concatenation (MaxLen=3), the least solution should include:
  // - x
  // - axb
  // - cxd
  //
  // A tensor implementation that loses correlation at projection time may
  // spuriously introduce cross terms (axd, cxb). The exact tensor mode must
  // match the base worklist solver.
  auto a = Exp::term(singleton("a"));
  auto b = Exp::term(singleton("b"));
  auto c = Exp::term(singleton("c"));
  auto d = Exp::term(singleton("d"));
  auto x = Exp::term(singleton("x"));

  E1 rhs =
      Exp::add(Exp::add(Exp::concat(a, "X", b), Exp::concat(c, "X", d)), x);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);

  BoundedLangSemiring::value_type expect = {"x", "axb", "cxd"};
  EXPECT_EQ(wl[0], expect);
}

TEST(NPA, TensorRegularizationPreservesNonZeroInitialSeeds) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  auto a = Exp::term(singleton("a"));
  auto b = Exp::term(singleton("b"));
  E1 rhs = Exp::add(Exp::hole("X"), Exp::concat(a, "X", b));

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {singleton("x")};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);
  BoundedLangSemiring::value_type expect = {"x", "axb"};
  EXPECT_EQ(wl[0], expect);
}

TEST(NPA, TensorConversionPreservesLeftAndRightCoefficientOrientation) {
  using D = BoundedLangSemiring;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;
  using Exp = npa::Exp1<D>;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> base_env;
  base_env["X"] = singleton("ab");
  base_env["F"] = singleton("ab");

  std::unordered_map<npa::Symbol, typename TD::value_type> tensor_env;
  tensor_env["X"] =
      npa::TensorSemiringTraits<D>::couple(singleton("a"), singleton("b"));
  tensor_env["F"] =
      npa::TensorSemiringTraits<D>::couple(singleton("a"), singleton("b"));

  auto left = Exp::seq(singleton("c"), Exp::hole("X"));
  auto right = Exp::seqR(Exp::hole("X"), singleton("d"));
  auto call = Exp::call("F", singleton("e"));

  auto left_base = npa::I1<D>::eval(false, base_env, left);
  auto right_base = npa::I1<D>::eval(false, base_env, right);
  auto call_base = npa::I1<D>::eval(false, base_env, call);

  auto left_tensor = npa::TensorSemiringTraits<D>::readout(npa::I1<TD>::eval(
      false, tensor_env, npa::Exp1ToTensor<D>::convert(left)));
  auto right_tensor = npa::TensorSemiringTraits<D>::readout(npa::I1<TD>::eval(
      false, tensor_env, npa::Exp1ToTensor<D>::convert(right)));
  auto call_tensor = npa::TensorSemiringTraits<D>::readout(npa::I1<TD>::eval(
      false, tensor_env, npa::Exp1ToTensor<D>::convert(call)));

  EXPECT_EQ(left_tensor, left_base);
  EXPECT_EQ(right_tensor, right_base);
  EXPECT_EQ(call_tensor, call_base);
  EXPECT_EQ(left_tensor, (D::value_type{"cab"}));
  EXPECT_EQ(right_tensor, (D::value_type{"abd"}));
  EXPECT_EQ(call_tensor, (D::value_type{"abe"}));
}

TEST(NPA, TensorTraitCoupleReadoutMatchesMatchedComposition) {
  using D = BoundedLangSemiring;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  auto a1 = D::one();
  auto b1 = singleton("b");
  auto a2 = singleton("c");
  auto b2 = D::one();

  auto coupled1 = npa::TensorSemiringTraits<D>::couple(a1, b1);
  auto coupled2 = npa::TensorSemiringTraits<D>::couple(a2, b2);
  auto composed = TD::extend(coupled1, coupled2);

  auto readout = npa::TensorSemiringTraits<D>::readout(composed);
  auto expected = D::extend(D::extend(a2, a1), D::extend(b1, b2));

  EXPECT_EQ(readout, expected);
  EXPECT_EQ(readout, (D::value_type{"cb"}));
}

TEST(NPA, TensorTarjanExtractsSelfContainedStar) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  E1 rhs = Exp::star(
      Exp::add(Exp::term(D::one()), Exp::seqR(Exp::bound("Z"), singleton("a"))),
      "Z");
  EXPECT_TRUE(npa::Exp1ToTensor<D>::is_regularizable(rhs));

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);
  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  rhs_tensor.emplace_back("X", npa::Exp1ToTensor<D>::convert(rhs));
  std::vector<typename TD::value_type> init_tensor = {TD::zero()};
  auto tarjan =
      npa::solve_linear_tensor_tarjan_impl<TD>(false, rhs_tensor, init_tensor);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  ASSERT_TRUE(tarjan.has_value());
  EXPECT_EQ(tp[0], wl[0]);
  EXPECT_EQ(npa::TensorSemiringTraits<D>::readout((*tarjan)[0]), wl[0]);
  EXPECT_EQ(tp[0], (D::value_type{"", "a", "aa", "aaa"}));
}

TEST(NPA, TensorRegularizationRejectsNonConstantStarForTarjanPath) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  E1 rhs = Exp::star(Exp::add(Exp::hole("X"), Exp::bound("Z")), "Z");

  EXPECT_FALSE(npa::Exp1ToTensor<D>::is_regularizable(rhs));
  EXPECT_TRUE(npa::Exp1ToTensor<D>::is_tensor_convertible(rhs));
}

TEST(NPA, TensorRegularizationSupportsNonConstantStarViaTensorizedFallback) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  E1 rhs = Exp::star(Exp::add(Exp::hole("X"), Exp::bound("Z")), "Z");

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {singleton("x")};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  rhs_tensor.emplace_back("X", npa::Exp1ToTensor<D>::convert(rhs));
  auto tensorized =
      npa::solve_linear_tensorized_impl<D>(false, rhs_tensor, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  ASSERT_EQ(tensorized.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);
  EXPECT_EQ(tp[0], tensorized[0]);
}

TEST(NPA, TensorRegularizationRejectsSubExpressions) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  E1 rhs = Exp::sub(Exp::term(BoundedLangSemiring::value_type{"a", "b"}),
                    Exp::term(BoundedLangSemiring::value_type{"b"}));
  EXPECT_FALSE(npa::Exp1ToTensor<D>::is_regularizable(rhs));

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = npa::solve_linear_tensor_impl<D>(false, eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);
  EXPECT_EQ(wl[0], (BoundedLangSemiring::value_type{"a"}));
}

TEST(NPA, TensorTarjanSupportsProjectedLeftLinearFragments) {
  using D = npa::PredicateRelationDomain;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  D::configure(2, 1);

  auto set_global_true = Exp::term(D::assignConst(0, true));
  auto set_local_true = Exp::term(D::assignConst(1, true));
  auto id = Exp::term(D::one());
  E1 rhs = Exp::add(
      Exp::project(Exp::concat(set_global_true, "X", set_local_true)), id);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  rhs_tensor.emplace_back("X", npa::Exp1ToTensor<D>::convert(rhs));
  std::vector<typename TD::value_type> init_tensor = {TD::zero()};
  auto tarjan =
      npa::solve_linear_tensor_tarjan_impl<TD>(false, rhs_tensor, init_tensor);

  ASSERT_TRUE(tarjan.has_value());
  ASSERT_EQ((*tarjan).size(), 1u);
  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_TRUE(D::equal(wl[0], tp[0]));
  EXPECT_TRUE(
      D::equal(npa::TensorSemiringTraits<D>::readout((*tarjan)[0]), wl[0]));
}

TEST(NPA, HighLevelTensorKeepsProjectedLeftLinearFragmentsOnTensorPath) {
  using D = npa::PredicateRelationDomain;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  D::configure(2, 1);

  auto set_global_true = Exp::term(D::assignConst(0, true));
  auto set_local_true = Exp::term(D::assignConst(1, true));
  auto id = Exp::term(D::one());
  E1 rhs = Exp::add(
      Exp::project(Exp::concat(set_global_true, "X", set_local_true)), id);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);

  testing::internal::CaptureStderr();
  auto tp = npa::solve_linear_tensor_impl<D>(true, eqns, init);
  std::string stderr_output = testing::internal::GetCapturedStderr();

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_TRUE(D::equal(wl[0], tp[0]));
  EXPECT_EQ(stderr_output.find("falling back"), std::string::npos);
}

TEST(NPA, TensorRegularizationSupportsCallTerms) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  E1 rhs = Exp::call("X", singleton("a"));

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {singleton("x")};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);
}

TEST(NPA, TensorRegularizationPreservesZeroRhsWithNonZeroSeed) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", Exp::term(D::zero()));

  std::vector<npa::DomVal<D>> init = {singleton("x")};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(wl[0], D::zero());
  EXPECT_EQ(tp[0], wl[0]);
}

TEST(NPA, TensorRegularizationSupportsCustomTensorTraits) {
  using D = CustomTensorLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  auto a = Exp::term(D::value_type{"a"});
  auto b = Exp::term(D::value_type{"b"});
  auto c = Exp::term(D::value_type{"c"});
  E1 rhs = Exp::add(Exp::concat(a, "X", b), c);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);
  auto tp = solve_with_unchecked_tensorized_helper<D>(eqns, init);

  ASSERT_EQ(wl.size(), 1u);
  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);
}

TEST(NPA, HighLevelTensorEntryFallsBackForUncheckedUtilityTraits) {
  using D = BoundedLangSemiring;
  using E1 = npa::E1<D>;
  using Exp = npa::Exp1<D>;

  auto a = Exp::term(singleton("a"));
  auto b = Exp::term(singleton("b"));
  auto c = Exp::term(singleton("c"));
  E1 rhs = Exp::add(Exp::concat(a, "X", b), c);

  std::vector<std::pair<npa::Symbol, E1>> eqns;
  eqns.emplace_back("X", rhs);

  std::vector<npa::DomVal<D>> init = {D::zero()};
  auto wl = npa::solve_linear_scc_impl<D>(false, eqns, init);

  testing::internal::CaptureStderr();
  auto tp = npa::solve_linear_tensor_impl<D>(true, eqns, init);
  std::string stderr_output = testing::internal::GetCapturedStderr();

  ASSERT_EQ(tp.size(), 1u);
  EXPECT_EQ(tp[0], wl[0]);
  EXPECT_NE(stderr_output.find("not paper-admissible"), std::string::npos);
}

TEST(NPA, DirectTensorDiffMatchesConvertedOrdinaryDiff) {
  using D = BoundedLangSemiring;
  using Exp0 = npa::Exp0<D>;
  using E0 = npa::E0<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["X"] = singleton("x");
  nu["F"] = singleton("f");

  E0 expr = Exp0::ndet(Exp0::call("F", Exp0::hole("X")),
                       Exp0::concat(Exp0::term(singleton("a")), "X",
                                    Exp0::term(singleton("b"))));

  (void)npa::I0<D>::eval(false, nu, expr);

  auto ordinary = npa::Exp1ToTensor<D>::convert(npa::Diff<D>::build(nu, expr));
  auto direct = npa::TensorDiff<D>::build(nu, expr);

  std::unordered_map<npa::Symbol, typename TD::value_type> env;
  env["X"] = npa::lift_base_value_to_tensor<D>(singleton("x"));
  env["F"] = npa::lift_base_value_to_tensor<D>(singleton("f"));

  auto ordinary_val = npa::I1<TD>::eval(false, env, ordinary);
  auto direct_val = npa::I1<TD>::eval(false, env, direct);
  EXPECT_TRUE(TD::equal(ordinary_val, direct_val));
}

TEST(NPA, DirectTensorDiffMatchesConvertedOrdinaryDiffAcrossCoreCases) {
  using D = BoundedLangSemiring;
  using Exp0 = npa::Exp0<D>;
  using E0 = npa::E0<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["X"] = singleton("x");
  nu["F"] = singleton("f");

  E0 seq_call = Exp0::seq(singleton("l"), Exp0::call("F", Exp0::hole("X")));
  E0 star = Exp0::star(
      Exp0::ndet(Exp0::term(D::one()),
                 Exp0::mul(Exp0::bound("Z"), Exp0::term(singleton("s")))),
      "Z");
  E0 concat =
      Exp0::concat(Exp0::term(singleton("a")), "X", Exp0::term(singleton("b")));
  E0 expr = Exp0::ndet(Exp0::mul(seq_call, Exp0::term(singleton("r"))),
                       Exp0::ndet(concat, star));

  (void)npa::I0<D>::eval(false, nu, expr);

  auto ordinary = npa::Exp1ToTensor<D>::convert(npa::Diff<D>::build(nu, expr));
  auto direct = npa::TensorDiff<D>::build(nu, expr);

  std::unordered_map<npa::Symbol, typename TD::value_type> env;
  env["X"] = npa::lift_base_value_to_tensor<D>(singleton("x"));
  env["F"] = npa::lift_base_value_to_tensor<D>(singleton("f"));

  auto ordinary_val = npa::I1<TD>::eval(false, env, ordinary);
  auto direct_val = npa::I1<TD>::eval(false, env, direct);
  EXPECT_TRUE(TD::equal(ordinary_val, direct_val));
}

TEST(NPA, NewtonInitUsesFOfBottom) {
  using D = BoundedLangSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", Exp0::term(singleton("a")));

  auto res = npa::NewtonSolver<D>::solve(eqns, false, 0);
  auto m = toMap<D>(res.first);

  EXPECT_EQ(m.at("X"), singleton("a"));
}

TEST(NPA, StarDifferentialEliminatesStarNodes) {
  using D = BoundedLangSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::unordered_map<npa::Symbol, npa::DomVal<D>> nu;
  nu["X"] = singleton("x");

  E0 expr = Exp0::star(Exp0::ndet(Exp0::hole("X"), Exp0::bound("Z")), "Z");
  (void)npa::I0<D>::eval(false, nu, expr);

  auto ordinary = npa::Diff<D>::build(nu, expr);
  auto tensor = npa::TensorDiff<D>::build(nu, expr);

  EXPECT_FALSE(npa::ExprFeatureDetector<D>::has_star(ordinary));
  EXPECT_FALSE(npa::ExprFeatureDetector<TD>::has_star(tensor));
}

TEST(NPA, NewtonRejectsMuEquations) {
  using D = BoundedLangSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", Exp0::mu(Exp0::term(singleton("a")), "Z"));

  EXPECT_THROW((void)npa::NewtonSolver<D>::solve(eqns, false, 1),
               npa::UnsupportedNewtonMuError);
}

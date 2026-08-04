// EAN M1 roundtrip test: importCanonical -> exportBatch must preserve
// semantics under an arbitrary Kleene-algebra interpretation, and must
// canonicalize choice into ACI form.
//
// The differential interpreter is a boolean 3x3 matrix Kleene algebra
// (join = OR, seq = boolean matmul, star = reflexive-transitive closure,
// zero = zero matrix, one = identity). Atoms (ints) map deterministically to
// generator matrices. Equality of the interpretation before/after roundtrip
// witnesses semantic preservation; this doubles as the seed of the RQ1
// differential-testing harness.

#include "Dataflow/APA/EAN/Export.h"
#include "Dataflow/APA/EAN/Import.h"

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

using elimination::PathExprFactory;
namespace ean = elimination::ean;

constexpr int N = 3;
struct Mat {
  std::array<std::array<bool, N>, N> a{};
  bool operator==(const Mat &o) const { return a == o.a; }
};
Mat zeroM() { return Mat{}; }
Mat oneM() {
  Mat m;
  for (int i = 0; i < N; ++i) m.a[i][i] = true;
  return m;
}
Mat orM(const Mat &x, const Mat &y) {
  Mat r;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) r.a[i][j] = x.a[i][j] || y.a[i][j];
  return r;
}
Mat mulM(const Mat &x, const Mat &y) {
  Mat r;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) {
      bool s = false;
      for (int k = 0; k < N; ++k) s = s || (x.a[i][k] && y.a[k][j]);
      r.a[i][j] = s;
    }
  return r;
}
Mat closureM(const Mat &x) {
  Mat r = oneM(), p = oneM();
  for (int it = 0; it < N + 1; ++it) {
    p = mulM(p, x);
    r = orM(r, p);
  }
  return r;
}
Mat gen(int id) {
  Mat m;
  std::uint32_t h = static_cast<std::uint32_t>(id) * 2654435761u + 0x9e3779b9u;
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) m.a[i][j] = ((h >> (i * N + j)) & 1u) != 0;
  return m;
}

using Ref = PathExprFactory<int>::Ref;
using Kind = PathExprFactory<int>::Kind;
Mat eval(const Ref &e) {
  switch (e->K) {
  case Kind::Zero: return zeroM();
  case Kind::One: return oneM();
  case Kind::Atom: return gen(*e->Transfer);
  case Kind::Union: return orM(eval(e->L), eval(e->R));
  case Kind::Concat: return mulM(eval(e->L), eval(e->R));
  case Kind::Star: return closureM(eval(e->L));
  }
  return zeroM();
}

// deterministic LCG for reproducible randomized expressions
std::uint64_t g_seed = 0x1234567;
std::uint32_t rnd() {
  g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
  return static_cast<std::uint32_t>(g_seed >> 33);
}
Ref randExpr(PathExprFactory<int> &F, int depth) {
  if (depth <= 0 || (rnd() % 3 == 0)) return F.atom(static_cast<int>(rnd() % 4));
  switch (rnd() % 4) {
  case 0: return F.unite(randExpr(F, depth - 1), randExpr(F, depth - 1));
  case 1: return F.concat(randExpr(F, depth - 1), randExpr(F, depth - 1));
  case 2: return F.star(randExpr(F, depth - 1));
  default: return F.atom(static_cast<int>(rnd() % 4));
  }
}

} // namespace

// Paper Eq.2 batch: r1 = (p·c)⊕(p·d), r2 = p·c, with p = a⊕b.
TEST(EanRoundtrip, PaperExampleBatchPreservesSemantics) {
  PathExprFactory<int> F;
  Ref a = F.atom(0), b = F.atom(1), c = F.atom(2), d = F.atom(3);
  Ref p = F.unite(a, b);
  Ref r1 = F.unite(F.concat(p, c), F.concat(p, d));
  Ref r2 = F.concat(p, c);

  auto imp = ean::importCanonical<int>({r1, r2});
  PathExprFactory<int> F2;
  auto out = ean::exportBatch<int>(imp, F2);

  ASSERT_EQ(out.size(), 2u);
  EXPECT_TRUE(eval(r1) == eval(out[0]));
  EXPECT_TRUE(eval(r2) == eval(out[1]));
}

// I2: choice is ACI, so a⊕b and b⊕a land in the same e-class.
TEST(EanRoundtrip, ChoiceIsCanonicalizedACI) {
  PathExprFactory<int> F;
  Ref a = F.atom(0), b = F.atom(1);
  Ref j1 = F.unite(a, b), j2 = F.unite(b, a);
  auto imp = ean::importCanonical<int>({j1, j2});
  EXPECT_EQ(imp.roots[0], imp.roots[1]);
}

TEST(EanRoundtrip, StarPreservesSemantics) {
  PathExprFactory<int> F;
  Ref a = F.atom(0), b = F.atom(1);
  Ref e = F.star(F.unite(a, F.concat(a, b)));
  auto imp = ean::importCanonical<int>({e});
  PathExprFactory<int> F2;
  auto out = ean::exportBatch<int>(imp, F2);
  EXPECT_TRUE(eval(e) == eval(out[0]));
}

TEST(EanRoundtrip, RandomizedDifferential) {
  for (int t = 0; t < 500; ++t) {
    PathExprFactory<int> F;
    Ref e = randExpr(F, 4);
    auto imp = ean::importCanonical<int>({e});
    PathExprFactory<int> F2;
    auto out = ean::exportBatch<int>(imp, F2);
    EXPECT_TRUE(eval(e) == eval(out[0])) << "random trial " << t;
  }
}

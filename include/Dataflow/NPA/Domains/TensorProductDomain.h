#ifndef NPA_TENSOR_PRODUCT_DOMAIN_H
#define NPA_TENSOR_PRODUCT_DOMAIN_H

#include "Dataflow/NPA/Core/NPACommon.h"

#include <utility>

namespace npa {

/**
 * TensorProductDomain<D> – utility paired domains for tensorized solving.
 *
 * This file provides:
 * - TensorProductDomain<D>: the classic *pair* construction (fast, but when
 *   used with readout R((a,b))=a⊗b after solving it can lose left/right
 *   correlation due to componentwise ⊕_p).
 * - TensorProductExactDomain<D>: an *exact correlated* representation for
 *   idempotent domains, modeling sums as finite sets of pairs so readout
 *   preserves correlation.
 *
 * These paired domains are useful implementation utilities, but by themselves
 * they are not the full admissible-semiring construction of TOPLAS 2016
 * Defn. 4.1. Paper-faithful tensor semantics come from a domain-specific
 * `TensorSemiringTraits<D>` specialization (for example, the predicate-relation
 * domain).
 */
template <class D> struct TensorProductDomain {
  using V = typename D::value_type;
  using value_type = std::pair<V, V>;
  using test_type = typename D::test_type;
  static constexpr bool idempotent = D::idempotent;

  static value_type zero() { return {D::zero(), D::zero()}; }
  static value_type one() { return {D::one(), D::one()}; }
  static bool equal(const value_type &a, const value_type &b) {
    return D::equal(a.first, b.first) && D::equal(a.second, b.second);
  }
  static value_type combine(const value_type &a, const value_type &b) {
    return {D::combine(a.first, b.first), D::combine(a.second, b.second)};
  }
  static value_type extend(const value_type &a, const value_type &b) {
    // Paper: (a1,b1) ⊗_p (a2,b2) = (a2⊗a1, b1⊗b2). So extend(a,b) with
    // a=(a1,b1), b=(a2,b2) must yield (a2⊗a1, b1⊗b2) so that
    // R(extend(p1,p2))=R(p2)⊗R(p1).
    return {D::extend(b.first, a.first), D::extend(a.second, b.second)};
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
    return {D::subtract(a.first, b.first), D::subtract(a.second, b.second)};
  }

  /** Project tensor value back to base domain (readout R((w1,w2)) = w1⊗w2). */
  static V project_left(const value_type &p) { return p.first; }
  static V project_right(const value_type &p) { return p.second; }
  /** Readout R((a,b)) = a⊗b (Reps et al. Alg. 3.4). */
  static V project(const value_type &p) { return D::extend(p.first, p.second); }
};

/**
 * TensorProductExactDomain<D> – exact correlated representation (idempotent).
 *
 * value_type is a finite set of pairs (stored as a vector with dedup).
 * combine() is set-union, extend() is pairwise ⊗_p, and project() is ⊕ over
 * readouts.
 *
 * This avoids the classic correlation loss where (a1,b1) ⊕_p (a2,b2) would
 * project to (a1⊕a2)⊗(b1⊕b2), introducing cross terms.
 */
template <class D> struct TensorProductExactDomain {
  using V = typename D::value_type;
  using pair_type = std::pair<V, V>;
  using value_type = std::vector<pair_type>;
  using test_type = typename D::test_type;
  static constexpr bool idempotent = true;

  static value_type zero() { return {}; }
  static value_type one() { return singleton(D::one(), D::one()); }

  static value_type singleton(const V &l, const V &r) {
    value_type out;
    out.emplace_back(l, r);
    return out;
  }

  static bool equal(const value_type &a, const value_type &b) {
    if (a.size() != b.size())
      return false;
    // Order-insensitive equality.
    for (const auto &pa : a) {
      bool found = false;
      for (const auto &pb : b) {
        if (D::equal(pa.first, pb.first) && D::equal(pa.second, pb.second)) {
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    }
    return true;
  }

  static value_type combine(value_type a, const value_type &b) {
    for (const auto &p : b)
      add_unique(a, p);
    return a;
  }

  static value_type extend(const value_type &a, const value_type &b) {
    // Tensor product: (a1,b1) ⊗_p (a2,b2) = (a2⊗a1, b1⊗b2).
    value_type out;
    for (const auto &p1 : a) {
      for (const auto &p2 : b) {
        pair_type p{D::extend(p2.first, p1.first),
                    D::extend(p1.second, p2.second)};
        add_unique(out, p);
      }
    }
    return out;
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

  static value_type subtract(const value_type &, const value_type &) {
    static_assert(
        D::idempotent,
        "TensorProductExactDomain is intended for idempotent domains");
    return {};
  }

  /** Readout R: ⊕ over pairs of (left ⊗ right). */
  static V project(const value_type &ps) {
    V out = D::zero();
    for (const auto &p : ps)
      out = D::combine(out, D::extend(p.first, p.second));
    return out;
  }

private:
  static void add_unique(value_type &vec, const pair_type &p) {
    // Dedup + dominance pruning under componentwise natural order:
    // If an existing pair (l2,r2) dominates (l1,r1), then its readout dominates
    // as well (by monotonicity), so removing the dominated pair is safe.
    for (size_t i = 0; i < vec.size();) {
      const auto &q = vec[i];
      const bool eq = domain_equal<D>(p.first, q.first) &&
                      domain_equal<D>(p.second, q.second);
      if (eq)
        return;
      const bool p_leq_q = domain_leq_idempotent<D>(p.first, q.first) &&
                           domain_leq_idempotent<D>(p.second, q.second);
      if (p_leq_q)
        return; // new pair is dominated
      const bool q_leq_p = domain_leq_idempotent<D>(q.first, p.first) &&
                           domain_leq_idempotent<D>(q.second, p.second);
      if (q_leq_p) {
        vec.erase(vec.begin() + i);
        continue;
      }
      ++i;
    }
    vec.push_back(p);
  }
};

} // namespace npa

#endif

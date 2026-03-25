#ifndef NPA_FIXPOINT_H
#define NPA_FIXPOINT_H

/**
 * \file
 * \brief Generic fixpoint iteration (Kleene-like).
 *
 * Used for:
 * (1) Kleene sequence κ^(i+1) = f(κ^(i)) (single variable);
 * (2) solving linear sub-systems (e.g. Star/Mu, or vector fixpoint for
 * the linearized system). NPA's Newton iteration uses these to compute
 * Δ^(i) as the least solution of Df|ν^(i)(X) + δ^(i) = X (Esparza et al.).
 */

#include "Dataflow/NPA/Core/NPACommon.h"

namespace npa {

/// Single-variable fixpoint: iterates until stable (κ^(i+1) = f(κ^(i))).
template <class D, class F> auto fix(bool verbose, DomVal<D> init, F f) {
  NPA_REQUIRE_DOMAIN(D);
  int cnt = 0;
  auto last = init;
  const int max_iters = domain_max_fixpoint_iters<D>();
  while (true) {
    auto nxt = f(last);
    if (domain_equal<D>(last, nxt)) {
      if (verbose)
        std::cerr << "[fp] " << cnt + 1 << "\n";
      return nxt;
    }
    last = std::move(nxt);
    ++cnt;
    if (max_iters >= 0 && cnt >= max_iters) {
      npa_note_fixpoint_limit_hit();
      if (verbose)
        std::cerr << "[fp] hit max_fixpoint_iters=" << max_iters << "\n";
      return last;
    }
  }
}

/// Vector fixpoint: iterates until all components stable (e.g. for linear
/// system in Naive strategy: update all variables each round).
template <class D, class Vec, class F>
Vec fix_vec(bool verbose, Vec init, F f) {
  int cnt = 0;
  const int max_iters = domain_max_fixpoint_iters<D>();
  while (true) {
    Vec nxt = f(init);
    bool stable = true;
    for (size_t i = 0; i < init.size(); ++i) {
      if (!domain_equal<D>(init[i], nxt[i])) {
        stable = false;
        break;
      }
    }
    if (stable) {
      if (verbose)
        std::cerr << "[fp] " << cnt + 1 << "\n";
      return nxt;
    }
    init.swap(nxt);
    ++cnt;
    if (max_iters >= 0 && cnt >= max_iters) {
      npa_note_fixpoint_limit_hit();
      if (verbose)
        std::cerr << "[fp] hit max_fixpoint_iters=" << max_iters << "\n";
      return init;
    }
  }
}

} // namespace npa

#endif // NPA_FIXPOINT_H

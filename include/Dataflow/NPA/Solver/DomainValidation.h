#ifndef NPA_SOLVER_DOMAIN_VALIDATION_H
#define NPA_SOLVER_DOMAIN_VALIDATION_H

#include "Dataflow/NPA/Core/Domain.h"

#include <iostream>

namespace npa {

template <class D>
inline bool run_basic_domain_contract_checks(bool verbose = false) {
  bool ok = true;
  const auto Zero = D::zero();
  const auto One = D::one();
  auto Require = [&](bool Condition, const char *Message) {
    if (Condition)
      return;
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] " << Message << '\n';
  };

  Require(D::equal(Zero, Zero), "zero() must equal itself");
  Require(D::equal(One, One), "one() must equal itself");
  Require(D::equal(D::combine(Zero, One), One),
          "zero must be a left identity for combine");
  Require(D::equal(D::combine(One, Zero), One),
          "zero must be a right identity for combine");
  Require(D::equal(D::combine(Zero, One), D::combine(One, Zero)),
          "combine must be commutative");
  Require(D::equal(D::extend(One, One), One),
          "one must be an identity for extend");
  Require(D::equal(D::extend(Zero, One), Zero),
          "zero must left-annihilate extend");
  Require(D::equal(D::extend(One, Zero), Zero),
          "zero must right-annihilate extend");
  Require(D::equal(D::extend_lin(One, One), D::extend(One, One)),
          "extend_lin must agree with extend on constants");
  Require(D::equal(D::ndetCombine(Zero, One), D::combine(Zero, One)),
          "ndetCombine must agree with combine");
  if (D::idempotent) {
    Require(D::equal(D::combine(Zero, Zero), Zero),
            "idempotent domain: zero⊕zero != zero");
    Require(D::equal(D::combine(One, One), One),
            "idempotent domain: one⊕one != one");
  }
  return ok;
}

} // namespace npa

#endif // NPA_SOLVER_DOMAIN_VALIDATION_H

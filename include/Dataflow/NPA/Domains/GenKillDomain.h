#ifndef NPA_GEN_KILL_TRANSFORMER_H
#define NPA_GEN_KILL_TRANSFORMER_H

#include "Dataflow/NPA/Core/Domain.h"
#include "Dataflow/NPA/Core/DomainExecution.h"

#include <utility>

#include <llvm/ADT/APInt.h>

namespace npa {

/**
 * GenKillTransformer – Transfer-function carrier for Gen/Kill problems
 * Elements are pairs (Kill, Gen) representing f(x) = (x \ Kill) U Gen
 *
 * Composition (extend):
 * f2(f1(x)) = ((x \ K1) U G1) \ K2) U G2
 *           = (x \ (K1 U K2)) U ((G1 \ K2) U G2)
 *
 * Join (combine):
 * f1(x) U f2(x) = ((x \ K1) U G1) U ((x \ K2) U G2)
 *               = (x \ (K1 n K2)) U (G1 U G2)
 */
class GenKillTransformer {
public:
  using value_type = std::pair<llvm::APInt, llvm::APInt>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  using width_context = DomainWidthContext<GenKillTransformer>;
  using RunState = typename width_context::state_type;
  using WidthScope = typename width_context::scope_type;

  // Additive identity (no paths): f(x) = 0  => Kill=all, Gen=0
  static value_type zero() {
    return zero(requireBitWidth());
  }

  static value_type zero(unsigned bit_width) {
    return {llvm::APInt::getAllOnes(bit_width), llvm::APInt(bit_width, 0)};
  }

  // Multiplicative identity (no-op): f(x) = x  => Kill=0, Gen=0
  static value_type one() {
    return one(requireBitWidth());
  }

  static value_type one(unsigned bit_width) {
    return {llvm::APInt(bit_width, 0), llvm::APInt(bit_width, 0)};
  }

  static bool equal(const value_type &a, const value_type &b) {
    return a.first == b.first && a.second == b.second;
  }

  static value_type combine(const value_type &a, const value_type &b) {
    // K_new = K_a & K_b
    // G_new = G_a | G_b
    return {a.first & b.first, a.second | b.second};
  }

  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }

  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }

  // extend(a, b) means "apply a AFTER b" (a o b)
  // a = outer, b = inner
  static value_type extend(const value_type &a, const value_type &b) {
    // K = K_inner | K_outer
    // G = (G_inner & ~K_outer) | G_outer
    return {b.first | a.first, (b.second & ~a.first) | a.second};
  }

  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }

  static value_type subtract(const value_type &a, const value_type &b) {
    // Not strictly needed for idempotent Newton if we don't use Sub expressions
    // But providing a safe behavior:
    // difference between functions is not well-defined in this lattice for
    // subtraction Just return a (safe over-approx?) or empty? Let's return zero
    // (identity) if unsure, but 'a' is safer if used in accumulation. Actually,
    // returning 'a' might prevent termination if used incorrectly. But standard
    // NPA for idempotent domains relies on ndetCombine.
    return a;
  }

private:
  static unsigned requireBitWidth() {
    return width_context::require(
        "GenKillTransformer width must be installed via WidthScope");
  }
};

} // namespace npa

namespace npa {
template <> struct DomainExecutionStateTraits<GenKillTransformer> {
  using state_type = GenKillTransformer::width_context::state_type;
  using scope_type = GenKillTransformer::width_context::scope_type;

  static state_type capture() {
    return GenKillTransformer::width_context::capture();
  }
};
} // namespace npa

#endif // NPA_GEN_KILL_TRANSFORMER_H

#ifndef NPA_BIT_VECTOR_DOMAIN_H
#define NPA_BIT_VECTOR_DOMAIN_H

#include "Dataflow/NPA/Core/NPACommon.h"
#include "Utils/LLVM/SystemHeaders.h"

#include <llvm/ADT/APInt.h>

namespace npa {

/**
 * BitSetDomain – idempotent semiring over APInt modelling sets of dataflow
 * facts combine : bitwise OR (\u222a) extend  : bitwise AND (\u2229) – path
 * concatenation keeps bits set on all steps zero    : all-zero vector (\u2205)
 * Note: width is fixed per analysis instance. Helper factory below creates
 *       sized elements so that static interface in NPA remains satisfied.
 */
class BitSetDomain {
public:
  using value_type = llvm::APInt;
  using test_type = bool; // no symbolic guards for now
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;
  using width_context = DomainWidthContext<BitSetDomain>;
  using RunState = typename width_context::state_type;
  using WidthScope = typename width_context::scope_type;

  static value_type zero() { return zero(requireBitWidth()); }
  static value_type zero(unsigned bit_width) { return llvm::APInt(bit_width, 0); }
  static value_type one() { return one(requireBitWidth()); }
  static value_type one(unsigned bit_width) {
    return llvm::APInt::getAllOnes(bit_width);
  }

  static bool equal(const value_type &a, const value_type &b) {
    return a.eq(b);
  }
  static value_type combine(const value_type &a, const value_type &b) {
    return a | b; /* union */
  }
  static value_type extend(const value_type &a, const value_type &b) {
    return a & b; /* intersection along path */
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(bool phi, const value_type &thenV,
                                const value_type &elseV) {
    return phi ? thenV : elseV;
  }
  static value_type subtract(const value_type &a, const value_type &b) {
    return a & (~b);
  }

private:
  static unsigned requireBitWidth() {
    return width_context::require(
        "BitSetDomain width must be installed via WidthScope");
  }
};

/// Backwards-compatible alias: older code may still refer to BitVectorDomain.
using BitVectorDomain = BitSetDomain;

template <> struct DomainExecutionStateTraits<BitSetDomain> {
  using state_type = BitSetDomain::width_context::state_type;
  using scope_type = BitSetDomain::width_context::scope_type;

  static state_type capture() { return BitSetDomain::width_context::capture(); }
};

} // namespace npa

#endif /* NPA_BIT_VECTOR_DOMAIN_H */

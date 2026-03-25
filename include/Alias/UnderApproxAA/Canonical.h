/**
 * @file Canonical.h
 * @brief Pointer canonicalization and equivalence checking utilities
 *
 * This file provides helper functions for normalizing pointer values and
 * detecting equivalence patterns. These utilities are used by the must-alias
 * analysis to identify when two pointers are guaranteed to refer to the same
 * memory location, despite syntactic differences in the IR.
 */

#ifndef UNDERAPPROX_CANONICAL_H
#define UNDERAPPROX_CANONICAL_H

#include <llvm/IR/Operator.h>

namespace llvm {
class DataLayout;
class Value;
} // namespace llvm

namespace UnderApprox {

/**
 * @brief Strip all no-op casts and invariant group intrinsics from a pointer
 *
 * Recursively removes operations that don't change the runtime address:
 * - Bitcasts (type changes without address changes)
 * - No-op address space casts
 * - Invariant group intrinsics (optimization hints)
 *
 * The result is a "canonical" form that can be used for comparison. This is
 * safe because these operations preserve the memory address - they only change
 * type metadata or optimization hints.
 *
 * @param V The pointer value to canonicalize
 * @return The canonical form of V (with no-op casts removed)
 *
 * Examples:
 *   - bitcast %p to i8*  →  %p (if cast is no-op)
 *   - launder_invariant_group(%p)  →  %p
 */
const llvm::Value *stripNoopCasts(const llvm::Value *V);

/**
 * @brief Check if two pointers have the same base and identical constant
 * offsets
 *
 * Uses LLVM's stripAndAccumulateInBoundsConstantOffsets to decompose each
 * pointer into base + offset. Two pointers must-alias if they have the same
 * base and identical offsets.
 *
 * @param DL The DataLayout for pointer size calculations
 * @param A First pointer value
 * @param B Second pointer value
 * @return true if A and B have the same base and constant offset
 *
 * Examples:
 *   - GEP(%base, 0, 5) and GEP(%base, 0, 5)  →  true
 *   - GEP(%base, 0, 5) and GEP(%base, 0, 6)  →  false
 *   - %base and GEP(%base, 0, 0)  →  true
 */
bool sameConstOffset(const llvm::DataLayout &DL, const llvm::Value *A,
                     const llvm::Value *B);

/**
 * @brief Check if a GEP has all zero indices
 *
 * A GEP with all zero indices is equivalent to its base pointer.
 *
 * @param V The value to check (should be a GEPOperator)
 * @return true if V is a GEP with all zero indices, false otherwise
 *
 * Examples:
 *   - GEP(%p, 0, 0)  →  true
 *   - GEP(%p, 0)     →  true
 *   - GEP(%p, 0, 1)  →  false
 */
bool isZeroGEP(const llvm::Value *V);

/**
 * @brief Check if two GEPs have the same base and identical index operands
 *
 * Two GEPs with the same source element type, same base pointer (after
 * stripping no-op casts), and identical index operands (same SSA values) must
 * alias. Sound under-approx: no false positives.
 *
 * @param A First pointer value (should be a GEP)
 * @param B Second pointer value (should be a GEP)
 * @return true if both are GEPs with same stripped base and same indices
 *
 * Examples:
 *   - GEP(%base, i) and GEP(%base, i)  →  true (same SSA i)
 *   - GEP(%base, 0, i) and GEP(%base, 0, i)  →  true
 */
bool sameGEPOperands(const llvm::Value *A, const llvm::Value *B);

/**
 * @brief Check if two values form a round-trip cast: inttoptr(ptrtoint(X)) ≡ X
 *
 * A pointer converted to an integer and back (with no arithmetic) is
 * guaranteed to be the same pointer. Both A and B must be pointer-typed.
 *
 * @param A First value (pointer type)
 * @param B Second value (pointer type)
 * @return true if one is inttoptr(ptrtoint(other)), false otherwise
 *
 * Example:
 *   %i = ptrtoint %p to i64
 *   %q = inttoptr %i to i8*
 *   isRoundTripCast(%q, %p)  →  true
 */
bool isRoundTripCast(const llvm::Value *A, const llvm::Value *B);

/**
 * @brief Check if an address space cast is a no-op (same source and dest space)
 *
 * Address space casts typically change the address space. However, if the
 * source and destination spaces are the same, the cast is a no-op.
 *
 * @param V The value to check (should be an AddrSpaceCastInst)
 * @return true if the cast is a no-op, false otherwise
 *
 * Note: In well-formed LLVM IR, address space casts should always change
 * the address space. However, intermediate optimization passes may create
 * no-op casts that should be canonicalized away.
 */
bool isNoopAddrSpaceCast(const llvm::Value *V);

/**
 * @brief Strip no-op arithmetic operations from an integer value
 *
 * Recursively removes arithmetic operations that don't change the value:
 * - Add 0, Sub 0
 * - Mul 1, Div 1
 * - Or 0, And -1
 *
 * This is used to detect enhanced round-trip patterns like:
 * inttoptr(ptrtoint(p) + 0) which is equivalent to p.
 *
 * @param V The integer value to simplify
 * @return The simplified value with no-op arithmetic removed
 */
const llvm::Value *stripNoopArithmetic(const llvm::Value *V);

/**
 * @brief Check for enhanced round-trip cast with no-op arithmetic
 *
 * Detects patterns like inttoptr(ptrtoint(p) + 0) or inttoptr(ptrtoint(p) | 0)
 * which are guaranteed to equal p. This extends the basic round-trip check
 * to handle intermediate no-op arithmetic.
 *
 * @param A First value (should be IntToPtrInst)
 * @param B Second value (the original pointer)
 * @return true if A is an enhanced round-trip of B, false otherwise
 *
 * Example:
 *   %i = ptrtoint %p to i64
 *   %j = add %i, 0
 *   %q = inttoptr %j to i8*
 *   isEnhancedRoundTrip(%q, %p) → true
 */
bool isEnhancedRoundTrip(const llvm::Value *A, const llvm::Value *B);

/**
 * @brief Check if a value is an allocation call (malloc, calloc, new, etc.)
 *
 * Used to identify heap allocation sites for same-allocation must-alias
 * detection. Two pointers derived from the same allocation call must alias.
 *
 * @param V The value to check
 * @return true if V is an allocation call, false otherwise
 */
bool isAllocationCall(const llvm::Value *V);

/**
 * @brief Check if two values derive from the same allocation site
 *
 * Two pointers that derive from the same allocation call (malloc, calloc, new)
 * must alias. This is sound because each allocation returns a unique address.
 *
 * @param S1 First pointer value
 * @param S2 Second pointer value
 * @return true if both derive from the same allocation site, false otherwise
 *
 * Example:
 *   %p = call i8* @malloc(i64 16)
 *   %q = bitcast i8* %p to i32*
 *   checkSameAllocationSite(%p, %q) → true
 */
bool checkSameAllocationSite(const llvm::Value *S1, const llvm::Value *S2);

} // end namespace UnderApprox
#endif

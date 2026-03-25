/**
 * @file Canonical.cpp
 * @brief Pointer canonicalization and equivalence checking utilities
 *
 * This file provides helper functions for normalizing pointer values and
 * detecting equivalence patterns. These utilities are used by the must-alias
 * analysis to identify when two pointers are guaranteed to refer to the same
 * memory location, despite syntactic differences.
 *
 * The canonicalization process strips away operations that don't change the
 * actual memory address at runtime, such as:
 * - Bitcasts (type changes without address changes)
 * - No-op address space casts
 * - Invariant group intrinsics (optimization hints, not actual address changes)
 */

#include "Alias/UnderApproxAA/Canonical.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>

using namespace llvm;
using namespace UnderApprox;

// ---------------------------------------------------------------------------
// Canonicalization helpers
// ---------------------------------------------------------------------------

/// Strip all no-op casts and invariant group intrinsics from a pointer value
///
/// This function recursively removes casts and intrinsics that don't change
/// the runtime address of a pointer. The result is a "canonical" form that
/// can be used for comparison.
///
/// @param V The pointer value to canonicalize
/// @return The canonical form of V (with no-op casts removed)
///
/// Examples:
///   - bitcast %p to i8*  →  %p (if %p is already i8*)
///   - addrspacecast %p from addrspace(0) to addrspace(0)  →  %p
///   - launder_invariant_group(%p)  →  %p
///
/// This is safe because these operations are guaranteed to preserve the
/// memory address - they only change type metadata or optimization hints.
const Value *UnderApprox::stripNoopCasts(const Value *V) {
  // Iteratively strip until no more no-op operations remain
  while (true) {
    // Strip bitcasts: casting between compatible pointer types doesn't
    // change the address (e.g., i32* → i8*, or i8* → i32*)
    if (auto *BC = dyn_cast<BitCastOperator>(V)) {
      V = BC->getOperand(0);
      continue;
    }

    // Strip no-op address space casts: casting within the same address space
    // is a no-op (though this shouldn't happen in well-formed IR, it's
    // possible in intermediate optimization states)
    if (isNoopAddrSpaceCast(V)) {
      V = cast<Operator>(V)->getOperand(0);
      continue;
    }

    // Strip all-zero GEP: pointer arithmetic with only zero indices is a no-op
    if (auto *GEP = dyn_cast<GEPOperator>(V))
      if (GEP->hasAllZeroIndices()) {
        V = GEP->getPointerOperand();
        continue;
      }

    // Strip invariant group intrinsics: these are optimization hints for
    // devirtualization and don't affect the actual memory address
    // - launder_invariant_group: marks a pointer as having a new "invariant
    // group"
    // - strip_invariant_group: removes invariant group metadata
    // Both are no-ops from an aliasing perspective
    if (auto *II = dyn_cast<IntrinsicInst>(V)) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::launder_invariant_group:
      case Intrinsic::strip_invariant_group:
        V = II->getArgOperand(0);
        continue;
      default:
        break;
      }
    }

    // No more no-op operations to strip - return canonical form
    return V;
  }
}

/// Check if an address space cast is a no-op (same source and destination
/// space)
///
/// Address space casts typically change the address space (e.g., from global
/// to local memory in GPU code). However, if the source and destination spaces
/// are the same, the cast is a no-op and can be stripped.
///
/// @param V The value to check (must be an AddrSpaceCastInst)
/// @return true if the cast is a no-op, false otherwise
///
/// Note: In well-formed LLVM IR, address space casts should always change
/// the address space. However, intermediate optimization passes may create
/// no-op casts that should be canonicalized away.
bool UnderApprox::isNoopAddrSpaceCast(const Value *V) {
  if (auto *ASC = dyn_cast<AddrSpaceCastOperator>(V)) {
    auto *SrcTy = dyn_cast<PointerType>(ASC->getOperand(0)->getType());
    auto *DstTy = dyn_cast<PointerType>(ASC->getType());
    if (!SrcTy || !DstTy)
      return false;
    return SrcTy->getAddressSpace() == DstTy->getAddressSpace();
  }
  return false;
}

/// Check if two pointers have the same base and identical constant offsets
///
/// This function uses LLVM's stripAndAccumulateInBoundsConstantOffsets to
/// decompose each pointer into a base + offset. Two pointers must-alias if
/// they have the same base and identical offsets.
///
/// @param DL The DataLayout for the target (needed for pointer size
/// calculations)
/// @param A First pointer value
/// @param B Second pointer value
/// @return true if A and B have the same base and constant offset, false
/// otherwise
///
/// Examples:
///   - GEP(%base, 0, 5) and GEP(%base, 0, 5)  →  true
///   - GEP(%base, 0, 5) and GEP(%base, 0, 6)  →  false
///   - %base and GEP(%base, 0, 0)  →  true (zero offset)
///   - GEP(%base, %var) and GEP(%base, %var)  →  false (non-constant offset)
///
/// This is a key rule for must-alias analysis: GEPs with identical constant
/// indices from the same base pointer must alias.
bool UnderApprox::sameConstOffset(const DataLayout &DL, const Value *A,
                                  const Value *B) {
  // Initialize offsets to zero
  APInt OffA(DL.getPointerSizeInBits(0), 0);
  APInt OffB(DL.getPointerSizeInBits(0), 0);

  // Strip casts and GEPs, accumulating constant offsets
  // Returns the base pointer (after stripping) and updates the offset
  const Value *BaseA = A->stripAndAccumulateInBoundsConstantOffsets(DL, OffA);
  const Value *BaseB = B->stripAndAccumulateInBoundsConstantOffsets(DL, OffB);

  // Must-alias if same base and same offset
  return BaseA == BaseB && OffA == OffB;
}

/// Check if a GEP has all zero indices
///
/// A GEP with all zero indices is equivalent to its base pointer. This is
/// a common pattern that should be recognized as must-alias.
///
/// @param V The value to check (must be a GEPOperator)
/// @return true if V is a GEP with all zero indices, false otherwise
///
/// Examples:
///   - GEP(%p, 0, 0)  →  true
///   - GEP(%p, 0)     →  true
///   - GEP(%p, 0, 1)  →  false
///   - %p (not a GEP) →  false
///
/// This is used by the atomic must-alias rules to detect when a GEP
/// is trivially equivalent to its base pointer.
bool UnderApprox::isZeroGEP(const Value *V) {
  if (auto *GEP = dyn_cast<GEPOperator>(V))
    return GEP->hasAllZeroIndices();
  return false;
}

/// Check if two GEPs have the same source element type, same base (after
/// stripping no-op casts), and identical index operands (same SSA values).
/// Sound under-approximation.
bool UnderApprox::sameGEPOperands(const Value *A, const Value *B) {
  auto *GEP1 = dyn_cast<GEPOperator>(A);
  auto *GEP2 = dyn_cast<GEPOperator>(B);
  if (!GEP1 || !GEP2)
    return false;
  if (GEP1->getSourceElementType() != GEP2->getSourceElementType())
    return false;
  if (stripNoopCasts(GEP1->getPointerOperand()) !=
      stripNoopCasts(GEP2->getPointerOperand()))
    return false;
  if (GEP1->getNumOperands() != GEP2->getNumOperands())
    return false;
  for (unsigned i = 1, e = GEP1->getNumOperands(); i < e; ++i)
    if (GEP1->getOperand(i) != GEP2->getOperand(i))
      return false;
  return true;
}

/// Check if two values form a round-trip cast: inttoptr(ptrtoint(X)) ≡ X
///
/// A pointer converted to an integer and back (with no arithmetic in between)
/// is guaranteed to be the same pointer. Both A and B must be pointer-typed
/// so that this rule can be used in must-alias seeding.
///
/// @param A First value (pointer type)
/// @param B Second value (pointer type)
/// @return true if one is inttoptr(ptrtoint(other)), false otherwise
///
/// Examples:
///   %i = ptrtoint %p to i64
///   %q = inttoptr %i to i8*
///   isRoundTripCast(%q, %p)  →  true
///
/// Limitation: Only detects direct round-trips (inttoptr directly uses
/// ptrtoint result). Patterns with arithmetic in between are not detected.
bool UnderApprox::isRoundTripCast(const Value *A, const Value *B) {
  auto *OpA = dyn_cast<Operator>(A);
  if (OpA && OpA->getOpcode() == Instruction::IntToPtr) {
    const Value *IntOp = OpA->getOperand(0);
    const Operator *OpInt = dyn_cast<Operator>(IntOp);
    if (OpInt && OpInt->getOpcode() == Instruction::PtrToInt &&
        OpInt->getOperand(0) == B)
      return true;
  }
  auto *OpB = dyn_cast<Operator>(B);
  if (OpB && OpB->getOpcode() == Instruction::IntToPtr) {
    const Value *IntOp = OpB->getOperand(0);
    const Operator *OpInt = dyn_cast<Operator>(IntOp);
    if (OpInt && OpInt->getOpcode() == Instruction::PtrToInt &&
        OpInt->getOperand(0) == A)
      return true;
  }
  return false;
}

/// Strip no-op arithmetic operations from an integer value
///
/// Removes operations that don't change the integer value:
/// - Add/Sub 0
/// - Mul/Div 1
/// - Or 0
/// - And -1 (all ones)
/// - Xor 0
///
/// This is used for enhanced round-trip detection.
const Value *UnderApprox::stripNoopArithmetic(const Value *V) {
  while (true) {
    auto *BO = dyn_cast<BinaryOperator>(V);
    if (!BO)
      break;

    const ConstantInt *C = nullptr;
    const Value *Other = nullptr;

    // Check for commutative operations (add, mul, or, and, xor)
    if (BO->isCommutative()) {
      // Try both operands for constant
      if (auto *C1 = dyn_cast<ConstantInt>(BO->getOperand(0))) {
        C = C1;
        Other = BO->getOperand(1);
      } else if (auto *C2 = dyn_cast<ConstantInt>(BO->getOperand(1))) {
        C = C2;
        Other = BO->getOperand(0);
      }
    } else {
      // Non-commutative: check operand 1 for constant
      if (auto *C2 = dyn_cast<ConstantInt>(BO->getOperand(1))) {
        C = C2;
        Other = BO->getOperand(0);
      }
    }

    if (!C || !Other)
      break;

    switch (BO->getOpcode()) {
    case Instruction::Add:
    case Instruction::Sub:
      if (C->isZero()) {
        V = Other;
        continue;
      }
      break;
    case Instruction::Mul:
      if (C->isOne()) {
        V = Other;
        continue;
      }
      break;
    case Instruction::SDiv:
    case Instruction::UDiv:
      if (C->isOne()) {
        V = Other;
        continue;
      }
      break;
    case Instruction::Or:
      if (C->isZero()) {
        V = Other;
        continue;
      }
      break;
    case Instruction::And:
      if (C->isMinusOne()) {
        V = Other;
        continue;
      }
      break;
    case Instruction::Xor:
      if (C->isZero()) {
        V = Other;
        continue;
      }
      break;
    default:
      break;
    }
    break;
  }
  return V;
}

/// Check for enhanced round-trip cast with no-op arithmetic
///
/// Detects inttoptr(ptrtoint(p) + 0) patterns which equal p.
/// This is sound because no-op arithmetic preserves the value.
bool UnderApprox::isEnhancedRoundTrip(const Value *A, const Value *B) {
  auto *ITP = dyn_cast<IntToPtrInst>(A);
  if (!ITP) {
    // Try the other direction
    ITP = dyn_cast<IntToPtrInst>(B);
    if (!ITP)
      return false;
    std::swap(A, B);
  }

  // Strip no-op arithmetic from the integer operand
  const Value *IntVal = stripNoopArithmetic(ITP->getOperand(0));

  // Check if we have ptrtoint of B (or a no-op cast of B)
  auto *PTI = dyn_cast<PtrToIntInst>(IntVal);
  if (!PTI)
    return false;

  // Compare with B after stripping casts
  return stripNoopCasts(PTI->getOperand(0)) == stripNoopCasts(B);
}

/// Check if a value is an allocation call (malloc, calloc, new, etc.)
///
/// Used to identify heap allocation sites for same-allocation must-alias.
bool UnderApprox::isAllocationCall(const Value *V) {
  auto *CB = dyn_cast<CallBase>(V);
  if (!CB)
    return false;

  const Function *F = CB->getCalledFunction();
  if (!F)
    return false;

  StringRef Name = F->getName();

  // Standard C allocation functions
  if (Name == "malloc" || Name == "calloc" || Name == "realloc" ||
      Name == "aligned_alloc" || Name == "valloc" || Name == "memalign")
    return true;

  // C++ operator new (various manglings)
  if (Name == "_Znwm" || Name == "_Znam" || // new, new[]
      Name == "_ZnwmRKSt9nothrow_t" ||      // nothrow new
      Name == "_ZnamRKSt9nothrow_t" ||
      Name == "_ZnwmSt11align_val_t" || // aligned new
      Name == "_ZnamSt11align_val_t")
    return true;

  // Check for builtin alloc functions
  if (Name.startswith("__builtin_") &&
      (Name.contains("alloc") || Name.contains("malloc")))
    return true;

  return false;
}

/// Check if two values derive from the same allocation site
///
/// Two pointers that derive from the same allocation call must alias.
/// This is sound because each allocation returns a unique address.
bool UnderApprox::checkSameAllocationSite(const Value *S1, const Value *S2) {
  const Value *U1 = getUnderlyingObject(S1);
  const Value *U2 = getUnderlyingObject(S2);

  // Must be the same underlying object
  if (U1 != U2)
    return false;

  // Must be an allocation call (malloc, calloc, new, etc.)
  // Note: AllocaInst and GlobalVariable are already handled elsewhere
  return isAllocationCall(U1);
}

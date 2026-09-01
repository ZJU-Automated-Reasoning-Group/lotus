/**
 * @file MemModels.cpp
 * @brief Memory model implementations for field-sensitive and field-insensitive
 * analysis.
 *
 * Provides canonicalizers for stripping pointer casts and offsets in both
 * field-sensitive and field-insensitive memory models. These help normalize
 * pointer values for pointer analysis.
 *
 * @author peiming
 */
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FICanonicalizer.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSCanonicalizer.h"
#include "Alias/InclusionBased/AserPTA/Util/Log.h"

#include <Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/Layout/ArrayLayout.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Operator.h>

using namespace aser;
using namespace llvm;

/**
 * @brief Strip null and undefined values, converting them to canonical forms.
 *
 * Converts null pointers to ConstantPointerNull and undefined values to
 * UndefValue with int8ptr type. Also handles inttoptr by converting to
 * universal pointer (undef).
 *
 * @param V The value to strip
 * @return Canonicalized value
 */
static const Value *stripNullOrUnDef(const Value *V) {
  // TODO: handle (inttoptr (ptrtoint %ptr)) pattern
  if (Operator::getOpcode(V) == Instruction::IntToPtr) {
    // inttoptr creates universal ptr, change it to
    V = UndefValue::get(Type::getInt8PtrTy(V->getContext()));
  }

  // a null ptr
  if (auto *C = dyn_cast<Constant>(V)) {
    if (C->isNullValue()) {
      V = ConstantPointerNull::get(Type::getInt8PtrTy(V->getContext()));
    }
  }

  // a uni ptr
  if (isa<UndefValue>(V)) {
    V = UndefValue::get(llvm::Type::getInt8PtrTy(V->getContext()));
  }
  return V;
}

/**
 * @brief Strip pointer casts and offsets for field-insensitive analysis.
 *
 * Modified from llvm::stripPointerCastsAndOffsets. Strips GEPs, bitcasts,
 * addrspace casts, and global aliases. Also handles intrinsic functions
 * like launder_invariant_group. Uses cycle detection to avoid infinite loops.
 *
 * @param V The value to strip
 * @return The base pointer value after stripping casts and offsets
 */
const Value *FICanonicalizer::stripPointerCastsAndOffsets(const Value *V) {
  // Even though we don't look through PHI nodes, we could be called on an
  // instruction in an unreachable block, which may be on a cycle.
  SmallPtrSet<const Value *, 4> Visited;
  Visited.insert(V);
  do {
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      // skip even if GEP is not in_bound
      V = GEP->getPointerOperand();
    } else if (Operator::getOpcode(V) == Instruction::BitCast ||
               Operator::getOpcode(V) == Instruction::AddrSpaceCast) {
      V = cast<Operator>(V)->getOperand(0);
    } else if (auto *GA = dyn_cast<GlobalAlias>(V)) {
      V = GA->getAliasee();
    } else {
      if (auto *CB = dyn_cast<CallBase>(V)) {
        if (const Value *RV = CB->getReturnedArgOperand()) {
          // the argument is also the return pointer,
          // this can increase both performance and accuarcy if it is
          // ever used but it seems no one use it
          V = RV;
          continue;
        }
        if (auto *II = dyn_cast<IntrinsicInst>(CB)) {
          if (II->getIntrinsicID() == Intrinsic::launder_invariant_group ||
              II->getIntrinsicID() == Intrinsic::strip_invariant_group) {
            V = CB->getArgOperand(0);
            continue;
          }
        }
      }
      return V;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  } while (Visited.insert(V).second);

  return V;
}

const Value *FICanonicalizer::canonicalize(const Value *V) {
  if (!V->getType()->isPointerTy())
    return V;
  V = stripPointerCastsAndOffsets(V);

  return stripNullOrUnDef(V);
}

/// Strip off pointer casts, all-zero GEPs, aliases and invariant group
/// info.
const Value *FSCanonicalizer::canonicalize(const llvm::Value *V) {
  if (!V->getType()->isPointerTy())
    return V;
  V = V->stripPointerCasts();

  return stripNullOrUnDef(V);
}

namespace aser {

// get the step size of the getelementptr (which uses variable index)
size_t getGEPStepSize(const GetElementPtrInst *GEP, const DataLayout &DL) {
  assert(!GEP->hasAllConstantIndices());
  // Iterate through GEP indices to find the first variable index.
  // The function handles GEPs with any number of operands by skipping
  // constant indices and returning the step size for the first variable index.

  for (gep_type_iterator GTI = gep_type_begin(GEP), GTE = gep_type_end(GEP);
       GTI != GTE; GTI++) {
    // Only ConstantInt is a statically fixed GEP index. Other Constant
    // subclasses (notably undef, poison, and constant expressions) make
    // hasAllConstantIndices() false and must be treated as symbolic indices.
    // Examples:
    //   getelementptr [type], [type *] %obj, 0, %var
    //   getelementptr [type], [type *] %obj, 0, 1, %var
    //   getelementptr [type], [type *] %obj, %var
    if (isa<ConstantInt>(GTI.getOperand())) {
      continue;
    }

    // Found the first variable index - return the step size for this indexed
    // type
    return DL.getTypeAllocSize(GTI.getIndexedType());
  }

  // Should we show source location of the unexpected instruction to user?
  LOG_ERROR("Encountered unexpected Instruction");
  LOG_DEBUG("Encountered unexpected GEP Instruction. inst={}", *GEP);
  // Be conservative in release and debug builds if LLVM introduces another
  // index form inconsistent with hasAllConstantIndices(). A byte stride avoids
  // crashing while keeping the field model over-approximating.
  return 1;
}

bool isArrayExistAtOffset(const std::map<size_t, ArrayLayout *> &arrayMap,
                          size_t pOffset, size_t elementSize) {
  if (arrayMap.empty()) {
    return false;
  }

  auto it = arrayMap.find(pOffset);
  if (it != arrayMap.end()) {
    ArrayLayout *arrLayout = it->second;
    if (arrLayout->getElementSize() == elementSize) {
      return true;
    } else if (arrLayout->getElementSize() < elementSize) {
      return false;
    }

    // the current layout is larger than the element size,
    // the underlying layout might be nested inside and is an array at zero
    // offset
    return isArrayExistAtOffset(arrLayout->getSubArrayMap(), 0, elementSize);
  } else {
    for (auto it : arrayMap) {
      size_t arrOffset = it.first;
      ArrayLayout *arrLayout = it.second;

      if (arrOffset < pOffset &&
          arrOffset + arrLayout->getArraySize() >= elementSize) {
        // might be nested here
        return isArrayExistAtOffset(arrLayout->getSubArrayMap(),
                                    pOffset - arrOffset, elementSize);
      }
    }
  }

  return false;
}

} // namespace aser

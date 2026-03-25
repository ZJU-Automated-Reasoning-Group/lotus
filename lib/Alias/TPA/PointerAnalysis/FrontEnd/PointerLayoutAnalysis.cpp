// Implementation of PointerLayoutAnalysis.
//
// Identifies all offsets within a type that contain pointers.
//
// Key Feature: Layout Propagation via Casts.
// Since pointers can be cast between different struct types (especially in C),
// we must ensure that the pointer analysis "sees" pointers even if they are
// accessed through a casted type.
//
// Algorithm:
// 1. Build initial layout: recursively scan types to find pointer fields.
// 2. Propagate layouts: Using the CastMap (from StructCastAnalysis), merge
// layout information.
//    If StructA is cast to StructB, then StructA effectively "has" pointers
//    where StructB does. (Conservative approach to handle unsafe casts).

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/PointerLayoutAnalysis.h"

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/CastMap.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeSet.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/PointerLayout.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

namespace {

class PtrLayoutMapBuilder {
private:
  const TypeSet &typeSet;
  PointerLayoutMap &ptrLayoutMap;

  void insertMap(const Type *, const PointerLayout *);

  const PointerLayout *processStructType(StructType *);
  const PointerLayout *processArrayType(ArrayType *);
  const PointerLayout *processPointerType(Type *);
  const PointerLayout *processNonPointerType(Type *);
  const PointerLayout *processType(Type *);

public:
  PtrLayoutMapBuilder(const TypeSet &t, PointerLayoutMap &p)
      : typeSet(t), ptrLayoutMap(p) {}

  void buildPtrLayoutMap();
};

void PtrLayoutMapBuilder::insertMap(const Type *type,
                                    const PointerLayout *layout) {
  ptrLayoutMap.insert(type, layout);
}

const PointerLayout *
PtrLayoutMapBuilder::processStructType(StructType *stType) {
  // We know nothing about opaque type. Conservatively treat it as a non-pointer
  // blob.
  if (stType->isOpaque()) {
    const auto *layout = PointerLayout::getEmptyLayout();
    insertMap(stType, layout);
    return layout;
  }

  util::VectorSet<size_t> ptrOffsets;

  const auto *structLayout = typeSet.getDataLayout().getStructLayout(stType);
  for (unsigned i = 0, e = stType->getNumElements(); i != e; ++i) {
    auto offset = structLayout->getElementOffset(i);
    auto *subType = stType->getElementType(i);
    const auto *subLayout = processType(subType);

    // Add offsets from sub-type, shifted by the field offset
    for (auto subOffset : *subLayout)
      ptrOffsets.insert(subOffset + offset);
  }

  const auto *stPtrLayout = PointerLayout::getLayout(std::move(ptrOffsets));
  insertMap(stType, stPtrLayout);
  return stPtrLayout;
}

const PointerLayout *
PtrLayoutMapBuilder::processArrayType(ArrayType *arrayType) {
  // For arrays, we just use the element layout.
  // NOTE: This assumes array accesses are collapsed to element 0.
  const auto *layout = processType(arrayType->getElementType());
  insertMap(arrayType, layout);
  return layout;
}

const PointerLayout *PtrLayoutMapBuilder::processPointerType(Type *ptrType) {
  const auto *layout = PointerLayout::getSinglePointerLayout();
  insertMap(ptrType, layout);
  return layout;
}

const PointerLayout *
PtrLayoutMapBuilder::processNonPointerType(Type *nonPtrType) {
  const auto *layout = PointerLayout::getEmptyLayout();
  insertMap(nonPtrType, layout);
  return layout;
}

const PointerLayout *PtrLayoutMapBuilder::processType(Type *type) {
  const auto *layout = ptrLayoutMap.lookup(type);
  if (layout != nullptr)
    return layout;

  if (auto *stType = dyn_cast<StructType>(type))
    return processStructType(stType);
  else if (auto *arrayType = dyn_cast<ArrayType>(type))
    return processArrayType(arrayType);
  else if (type->isPointerTy() || type->isFunctionTy())
    return processPointerType(type);
  else
    return processNonPointerType(type);
}

void PtrLayoutMapBuilder::buildPtrLayoutMap() {
  for (auto *type : typeSet)
    processType(type);
}

// Propagates pointer layout information across bitcasts.
class PtrLayoutMapPropagator {
private:
  const CastMap &castMap;
  PointerLayoutMap &ptrLayoutMap;

public:
  PtrLayoutMapPropagator(const CastMap &c, PointerLayoutMap &p)
      : castMap(c), ptrLayoutMap(p) {}

  void propagatePtrLayoutMap();
};

void PtrLayoutMapPropagator::propagatePtrLayoutMap() {
  // Fix #5: The cast map records edges srcType -> {dstType, ...}, meaning
  // "a pointer to srcType is cast to a pointer to dstType". When such a cast
  // exists, code that accesses memory through dstType may actually be reading
  // srcType memory (and vice versa). To be sound we must propagate pointer
  // layout information in BOTH directions:
  //
  //   - Forward (src -> dst): if srcType has a pointer at offset X, then
  //     dstType should also be considered to have a pointer at offset X,
  //     because code that casts srcType* to dstType* and then reads a field
  //     at offset X will be reading a pointer.
  //   - Backward (dst -> src): if dstType has a pointer at offset X, then
  //     srcType should also be considered to have a pointer at offset X,
  //     because code that casts srcType* to dstType* and writes a pointer at
  //     offset X is writing into srcType memory.
  //
  // The previous implementation only merged RHS (dstType) layouts into LHS
  // (srcType), which is the backward direction only. This missed the forward
  // direction, causing pointer fields of the cast-to type to be invisible when
  // accessed through the original type.
  //
  // We perform a two-pass approach:
  //   Pass 1: collect all merged layouts without modifying the map (to avoid
  //           order-dependent results).
  //   Pass 2: write the merged layouts back.

  // Collect updates: for each type, the merged layout to apply.
  llvm::DenseMap<const llvm::Type *, const PointerLayout *> updates;

  for (auto const &mapping : castMap) {
    auto *srcType = mapping.first; // the type being cast FROM
    const auto *srcLayout = ptrLayoutMap.lookup(srcType);
    assert(srcLayout != nullptr && "Cannot find ptrLayout for src type");

    for (auto *dstType : mapping.second) {
      const auto *dstLayout = ptrLayoutMap.lookup(dstType);
      assert(dstLayout != nullptr && "Cannot find ptrLayout for dst type");

      // Forward: merge srcType's layout into dstType.
      auto itrDst = updates.find(dstType);
      const auto *curDst =
          (itrDst != updates.end()) ? itrDst->second : dstLayout;
      updates[dstType] = PointerLayout::merge(curDst, srcLayout);

      // Backward: merge dstType's layout into srcType.
      auto itrSrc = updates.find(srcType);
      const auto *curSrc =
          (itrSrc != updates.end()) ? itrSrc->second : srcLayout;
      updates[srcType] = PointerLayout::merge(curSrc, dstLayout);
    }
  }

  // Write back all merged layouts.
  for (auto const &kv : updates)
    ptrLayoutMap.insert(kv.first, kv.second);
}

} // namespace

PointerLayoutMap PointerLayoutAnalysis::runOnTypes(const TypeSet &typeSet) {
  PointerLayoutMap ptrLayoutMap;

  // Phase 1: Structural analysis
  PtrLayoutMapBuilder(typeSet, ptrLayoutMap).buildPtrLayoutMap();

  // Phase 2: Propagation via casts
  PtrLayoutMapPropagator(castMap, ptrLayoutMap).propagatePtrLayoutMap();

  return ptrLayoutMap;
}

} // namespace tpa

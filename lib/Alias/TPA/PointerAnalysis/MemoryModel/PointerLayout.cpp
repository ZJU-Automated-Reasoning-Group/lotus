// Implementation of PointerLayout.
//
// PointerLayout tracks which offsets within a type contain pointers.
// This is essential for:
// 1. Scanning memory to find pointers (e.g., during copying or garbage
// collection simulation).
// 2. Precision: Knowing that an offset does NOT contain a pointer allows us to
// ignore it.
//
// Data Structure:
// - A set of offsets (`validOffsets`).
// - Supports merging layouts (union of offsets).

#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/PointerLayout.h"

namespace tpa {

const PointerLayout *PointerLayout::getEmptyLayout() { return emptyLayout; }

// Returns a layout for a type that is a single pointer (offset 0 is a pointer).
const PointerLayout *PointerLayout::getSinglePointerLayout() {
  return singlePointerLayout;
}

const PointerLayout *PointerLayout::getLayout(SetType &&set) {
  auto itr = layoutSet.insert(PointerLayout(std::move(set))).first;
  return &(*itr);
}

const PointerLayout *
PointerLayout::getLayout(std::initializer_list<size_t> ilist) {
  SetType set(ilist);
  return getLayout(std::move(set));
}

// Merges two pointer layouts (union).
// Used when analyzing aggregate types or merging types in union/cast scenarios.
const PointerLayout *PointerLayout::merge(const PointerLayout *lhs,
                                          const PointerLayout *rhs) {
  assert(lhs != nullptr && rhs != nullptr);

  if (lhs == rhs)
    return lhs;
  if (lhs->empty())
    return rhs;
  if (rhs->empty())
    return lhs;

  SetType newSet(lhs->validOffsets);
  newSet.merge(rhs->validOffsets);
  return getLayout(std::move(newSet));
}

} // namespace tpa
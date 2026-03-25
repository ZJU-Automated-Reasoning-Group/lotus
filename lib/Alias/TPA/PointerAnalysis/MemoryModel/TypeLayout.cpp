// Implementation of TypeLayout.
//
// TypeLayout aggregates the structural information of a type for analysis
// purposes. It combines:
// 1. Size: Total size in bytes.
// 2. ArrayLayout: Where the array regions are.
// 3. PointerLayout: Where the pointers are.
//
// This class acts as the query interface for memory operations to understand
// the geometry of the objects they are accessing.

#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

namespace tpa {

const TypeLayout *
TypeLayout::getTypeLayout(size_t s, std::initializer_list<ArrayTriple> a,
                          std::initializer_list<size_t> p) {
  return getTypeLayout(s, ArrayLayout::getLayout(std::move(a)),
                       PointerLayout::getLayout(std::move(p)));
}

const TypeLayout *TypeLayout::getTypeLayout(size_t size, const ArrayLayout *a,
                                            const PointerLayout *p) {
  assert(a != nullptr && p != nullptr);

  auto itr = typeSet.insert(TypeLayout(size, a, p)).first;
  return &(*itr);
}

// Creates a TypeLayout for an array of 'elemCount' elements of 'elemLayout'.
// Combines the element layouts into a larger layout, adjusting offsets.
const TypeLayout *TypeLayout::getArrayTypeLayout(const TypeLayout *elemLayout,
                                                 size_t elemCount) {
  assert(elemLayout != nullptr);
  const auto *const elemArrayLayout = elemLayout->getArrayLayout();
  const auto elemSize = elemLayout->getSize();
  const auto newSize = elemSize * elemCount;

  // Create a new array triple covering the entire new array
  auto arrayTripleList = ArrayLayout::ArrayTripleList();
  arrayTripleList.reserve(elemArrayLayout->size() + 1);
  arrayTripleList.push_back({0, newSize, elemSize});

  // Inherit existing array structures from the element (not strictly necessary
  // if we collapse everything to the new top-level array, but good for nested
  // precision?)
  for (auto const &triple : *elemArrayLayout)
    arrayTripleList.push_back(triple);

  const auto *const newArrayLayout =
      ArrayLayout::getLayout(std::move(arrayTripleList));

  // For pointers, we usually reuse the element's pointer layout because
  // accessing the array will be collapsed to accessing the first element.
  return getTypeLayout(newSize, newArrayLayout, elemLayout->getPointerLayout());
}

// Factory for a type that is just a pointer.
const TypeLayout *TypeLayout::getPointerTypeLayoutWithSize(size_t size) {
  return getTypeLayout(size, ArrayLayout::getDefaultLayout(),
                       PointerLayout::getSinglePointerLayout());
}

// Factory for a scalar type (no internal structure).
const TypeLayout *TypeLayout::getNonPointerTypeLayoutWithSize(size_t size) {
  return getTypeLayout(size, ArrayLayout::getDefaultLayout(),
                       PointerLayout::getEmptyLayout());
}

// Factory for a byte array (e.g., char[] or unknown buffer).
const TypeLayout *TypeLayout::getByteArrayTypeLayout() {
  return getTypeLayout(1, ArrayLayout::getByteArrayLayout(),
                       PointerLayout::getSinglePointerLayout());
}

// Delegates offset calculation to the contained ArrayLayout.
std::pair<size_t, bool> TypeLayout::offsetInto(size_t size) const {
  return arrayLayout->offsetInto(size);
}

} // namespace tpa
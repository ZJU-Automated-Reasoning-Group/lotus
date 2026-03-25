// Implementation of ArrayLayout.
//
// ArrayLayout describes the regions within a type that correspond to arrays.
// This is critical for handling array indexing, where multiple concrete offsets
// need to be collapsed into a single "summary" offset to keep analysis finite.
//
// Data Structure:
// - A list of `ArrayTriple`s: {start, end, element_size}.
// - Example: struct { int x; int arr[10]; int y; }
//   - x: offset 0, size 4.
//   - arr: offset 4, size 40 (10 * 4). Triple: {4, 44, 4}.
//   - y: offset 44, size 4.
//
// Logic:
// - `offsetInto`: Checks if a raw byte offset falls within any array region.
//   If so, it normalizes the offset to the first element (modulo arithmetic).
//   e.g., accessing arr[3] (offset 4 + 3*4 = 16) -> maps to offset 4.

#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/ArrayLayout.h"

#include <limits>

namespace tpa {

// Validates the integrity of the array triples list.
// - Intervals must be well-formed (start + size <= end).
// - Length must be a multiple of element size.
// - List must be sorted and non-overlapping.
static bool validateTripleList(const ArrayLayout::ArrayTripleList &list) {
  for (auto const &triple : list) {
    if (triple.start + triple.size > triple.end)
      return false;
    if ((triple.end - triple.start) % triple.size != 0)
      return false;
  }

  auto const isSorted = std::is_sorted(
      list.begin(), list.end(), [](auto const &lhs, auto const &rhs) {
        return (lhs.start < rhs.start) ||
               (lhs.start == rhs.start && lhs.size > rhs.size);
      });

  if (!isSorted)
    return false;

  return std::unordered_set<ArrayTriple>(list.begin(), list.end()).size() ==
         list.size();
}

const ArrayLayout *ArrayLayout::getLayout(ArrayTripleList &&list) {
  assert(validateTripleList(list));
  auto itr = layoutSet.insert(ArrayLayout(std::move(list))).first;
  return &(*itr);
}

const ArrayLayout *
ArrayLayout::getLayout(std::initializer_list<ArrayTriple> ilist) {
  ArrayTripleList list(ilist);
  return getLayout(std::move(list));
}

// Layout for a generic byte array (char* or unknown size).
// Treats the entire range as one large array of bytes.
const ArrayLayout *ArrayLayout::getByteArrayLayout() {
  return getLayout({{0, std::numeric_limits<size_t>::max(), 1}});
}

const ArrayLayout *ArrayLayout::getDefaultLayout() { return defaultLayout; }

// Maps a raw offset to a normalized offset.
// Returns: {new_offset, is_array_access}
// If the offset falls into an array triple, returns the offset of the base
// element and true. Otherwise returns original offset and false.
std::pair<size_t, bool> ArrayLayout::offsetInto(size_t offset) const {
  bool hitArray = false;
  for (auto const &triple : arrayLayout) {
    if (triple.start > offset)
      break;

    if (triple.start <= offset && offset < triple.end) {
      hitArray = true;
      // Normalize: map to the start of the array + offset within the element.
      // Actually, here it maps to the specific element slot within the *first*
      // index? Logic: start + (off - start) % size. Example: start=0, size=4.
      // Access 8. 0 + (8-0)%4 = 0. Example: start=0, size=4. Access 9 (byte 1
      // of index 2). 0 + (9-0)%4 = 1. So it preserves intra-element offset but
      // collapses indices.
      offset = triple.start + (offset - triple.start) % triple.size;
    }
  }
  return std::make_pair(offset, hitArray);
}

} // namespace tpa
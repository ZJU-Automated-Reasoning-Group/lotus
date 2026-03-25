#pragma once

#include "Alias/TPA/Util/Hashing.h"

#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <unordered_set>
#include <vector>

namespace tpa {

struct ArrayTriple {
  size_t start, end, size;
};

inline bool operator==(const ArrayTriple &lhs, const ArrayTriple &rhs) {
  return lhs.start == rhs.start && lhs.end == rhs.end && lhs.size == rhs.size;
}

class ArrayLayout {
public:
  using ArrayTripleList = std::vector<ArrayTriple>;

private:
  ArrayTripleList arrayLayout;

  static std::unordered_set<ArrayLayout> layoutSet;
  static const ArrayLayout *defaultLayout;

  ArrayLayout(ArrayTripleList &&l) noexcept : arrayLayout(std::move(l)) {}

public:
  using const_iterator = ArrayTripleList::const_iterator;

  ArrayLayout(const ArrayLayout &) = delete;
  ArrayLayout(ArrayLayout &&) noexcept = default;
  ArrayLayout &operator=(const ArrayLayout &) = delete;
  ArrayLayout &operator=(ArrayLayout &&) = delete;

  std::pair<size_t, bool> offsetInto(size_t offset) const;

  bool operator==(const ArrayLayout &rhs) const {
    return arrayLayout == rhs.arrayLayout;
  }
  bool operator!=(const ArrayLayout &rhs) const { return !(*this == rhs); }

  const_iterator begin() const { return arrayLayout.begin(); }
  const_iterator end() const { return arrayLayout.end(); }
  bool empty() const { return arrayLayout.empty(); }
  size_t size() const { return arrayLayout.size(); }

  // Default layout: scalar or struct that doesn't contain any array
  static const ArrayLayout *getDefaultLayout();
  // ByteArray layout: the most conservative layout that assumes an array of
  // byte
  static const ArrayLayout *getByteArrayLayout();
  // The most general way of constructing a layout
  static const ArrayLayout *getLayout(ArrayTripleList &&list);
  static const ArrayLayout *getLayout(std::initializer_list<ArrayTriple>);

  friend struct std::hash<ArrayLayout>;
};

} // namespace tpa

namespace std {

template <> struct hash<tpa::ArrayTriple> {
  size_t operator()(const tpa::ArrayTriple &triple) const {
    return util::hashTriple(triple.start, triple.end, triple.size);
  }
};

template <> struct hash<tpa::ArrayLayout> {
  size_t operator()(const tpa::ArrayLayout &layout) const {
    return util::ContainerHasher<tpa::ArrayLayout::ArrayTripleList>()(
        layout.arrayLayout);
  }
};

} // namespace std

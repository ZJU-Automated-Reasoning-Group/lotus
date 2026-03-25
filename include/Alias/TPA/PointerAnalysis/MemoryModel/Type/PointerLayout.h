#pragma once

#include "Alias/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/TPA/Util/Hashing.h"

#include <unordered_set>

namespace tpa {

class PointerLayout {
private:
  using SetType = util::VectorSet<size_t>;
  SetType validOffsets;

  static std::unordered_set<PointerLayout> layoutSet;
  static const PointerLayout *emptyLayout;
  static const PointerLayout *singlePointerLayout;

  PointerLayout(SetType &&s) : validOffsets(std::move(s)) {}

public:
  using const_iterator = typename SetType::const_iterator;

  PointerLayout(const PointerLayout &) = delete;
  PointerLayout(PointerLayout &&) noexcept = default;
  PointerLayout &operator=(const PointerLayout &) = delete;
  PointerLayout &operator=(PointerLayout &&) = delete;

  bool operator==(const PointerLayout &rhs) const {
    return validOffsets == rhs.validOffsets;
  }
  bool operator!=(const PointerLayout &rhs) const { return !(*this == rhs); }

  const_iterator lower_bound(size_t offset) const {
    return validOffsets.lower_bound(offset);
  }

  size_t size() const { return validOffsets.size(); }
  bool empty() const { return validOffsets.empty(); }
  const_iterator begin() const { return validOffsets.begin(); }
  const_iterator end() const { return validOffsets.end(); }

  // Empty layout: types that are non-pointer
  static const PointerLayout *getEmptyLayout();
  // SinglePointer layout:: for pointer types
  static const PointerLayout *getSinglePointerLayout();
  // The most general way of constructing a layout
  static const PointerLayout *getLayout(SetType &&list);
  static const PointerLayout *getLayout(std::initializer_list<size_t>);

  static const PointerLayout *merge(const PointerLayout *,
                                    const PointerLayout *);

  friend struct std::hash<PointerLayout>;
};

} // namespace tpa

namespace std {

template <> struct hash<tpa::PointerLayout> {
  size_t operator()(const tpa::PointerLayout &layout) const {
    return util::ContainerHasher<tpa::PointerLayout::SetType>()(
        layout.validOffsets);
  }
};

} // namespace std

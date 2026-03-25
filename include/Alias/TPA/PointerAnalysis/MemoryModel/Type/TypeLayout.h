#pragma once

#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/ArrayLayout.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/PointerLayout.h"
#include "Alias/TPA/Util/Hashing.h"

#include <unordered_set>

namespace tpa {

class TypeLayout {
private:
  size_t size;
  const ArrayLayout *arrayLayout;
  const PointerLayout *ptrLayout;

  static std::unordered_set<TypeLayout> typeSet;

  TypeLayout(size_t s, const ArrayLayout *a, const PointerLayout *p)
      : size(s), arrayLayout(a), ptrLayout(p) {}

public:
  TypeLayout(const TypeLayout &) = delete;
  TypeLayout(TypeLayout &&) noexcept = default;
  TypeLayout &operator=(const TypeLayout &) = delete;
  TypeLayout &operator=(TypeLayout &&) = delete;

  size_t getSize() const { return size; }
  const ArrayLayout *getArrayLayout() const { return arrayLayout; }
  const PointerLayout *getPointerLayout() const { return ptrLayout; }

  std::pair<size_t, bool> offsetInto(size_t size) const;

  bool operator==(const TypeLayout &rhs) const {
    return size == rhs.size && arrayLayout == rhs.arrayLayout &&
           ptrLayout == rhs.ptrLayout;
  }
  bool operator!=(const TypeLayout &rhs) const { return !(*this == rhs); }

  static const TypeLayout *getByteArrayTypeLayout();
  static const TypeLayout *getPointerTypeLayoutWithSize(size_t s);
  static const TypeLayout *getNonPointerTypeLayoutWithSize(size_t s);
  static const TypeLayout *getArrayTypeLayout(const TypeLayout *,
                                              size_t elemCount);
  static const TypeLayout *getTypeLayout(size_t s, const ArrayLayout *,
                                         const PointerLayout *);
  static const TypeLayout *getTypeLayout(size_t s,
                                         std::initializer_list<ArrayTriple>,
                                         std::initializer_list<size_t>);

  friend struct std::hash<TypeLayout>;
};

} // namespace tpa

namespace std {

template <> struct hash<tpa::TypeLayout> {
  size_t operator()(const tpa::TypeLayout &type) const {
    return util::hashTriple(type.size, type.arrayLayout, type.ptrLayout);
  }
};

} // namespace std

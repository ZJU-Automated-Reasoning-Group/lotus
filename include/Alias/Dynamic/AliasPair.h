#pragma once

#include "Alias/Dynamic/DynamicPointer.h"

#include <utility>

#include <llvm/ADT/DenseMapInfo.h>

namespace dynamic {

class AliasPair {
private:
  DynamicPointer first, second;

public:
  AliasPair(DynamicPointer p0, DynamicPointer p1) {
    if (p0 < p1) {
      first = p0;
      second = p1;
    } else {
      second = p0;
      first = p1;
    }
  }

  DynamicPointer getFirst() const { return first; }
  DynamicPointer getSecond() const { return second; }
};

inline bool operator==(const AliasPair &lhs, const AliasPair &rhs) {
  return lhs.getFirst() == rhs.getFirst() && lhs.getSecond() == rhs.getSecond();
}
inline bool operator!=(const AliasPair &lhs, const AliasPair &rhs) {
  return !(lhs == rhs);
}
inline bool operator<(const AliasPair &lhs, const AliasPair &rhs) {
  return (lhs.getFirst() < rhs.getFirst()) ||
         (lhs.getFirst() == rhs.getFirst() &&
          lhs.getSecond() < rhs.getSecond());
}
inline bool operator>=(const AliasPair &lhs, const AliasPair &rhs) {
  return !(lhs < rhs);
}
inline bool operator>(const AliasPair &lhs, const AliasPair &rhs) {
  return rhs < lhs;
}
inline bool operator<=(const AliasPair &lhs, const AliasPair &rhs) {
  return !(rhs < lhs);
}
} // namespace dynamic

namespace llvm {
template <> struct DenseMapInfo<dynamic::AliasPair> {
  static inline dynamic::AliasPair getEmptyKey() {
    return dynamic::AliasPair(
        DenseMapInfo<dynamic::DynamicPointer>::getEmptyKey(),
        DenseMapInfo<dynamic::DynamicPointer>::getEmptyKey());
  }
  static inline dynamic::AliasPair getTombstoneKey() {
    return dynamic::AliasPair(
        DenseMapInfo<dynamic::DynamicPointer>::getTombstoneKey(),
        DenseMapInfo<dynamic::DynamicPointer>::getTombstoneKey());
  }
  static unsigned getHashValue(const dynamic::AliasPair &pair) {
    return DenseMapInfo<
        std::pair<dynamic::DynamicPointer, dynamic::DynamicPointer>>::
        getHashValue(std::make_pair(pair.getFirst(), pair.getSecond()));
  }
  static bool isEqual(const dynamic::AliasPair &lhs,
                      const dynamic::AliasPair &rhs) {
    return lhs == rhs;
  }
};
} // namespace llvm
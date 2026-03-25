#pragma once

#include "Alias/TPA/Util/DataStructure/VectorSet.h"

#include <unordered_map>

namespace llvm {
class Type;
} // namespace llvm

namespace tpa {

class CastMap {
private:
  using SetType = util::VectorSet<llvm::Type *>;
  using MapType = std::unordered_map<llvm::Type *, SetType>;
  MapType castMap;

public:
  using iterator = MapType::iterator;
  using const_iterator = MapType::const_iterator;

  void insert(llvm::Type *src, llvm::Type *dst) { castMap[src].insert(dst); }

  iterator find(llvm::Type *src) { return castMap.find(src); }

  SetType &getOrCreateRHS(llvm::Type *src) { return castMap[src]; }

  iterator begin() { return castMap.begin(); }
  iterator end() { return castMap.end(); }
  const_iterator begin() const { return castMap.begin(); }
  const_iterator end() const { return castMap.end(); }
};

} // namespace tpa
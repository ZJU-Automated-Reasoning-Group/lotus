#pragma once

#include <llvm/ADT/DenseMap.h>

namespace tpa {

template <typename KeyType, typename ValueType> class ConstPointerMap {
private:
  using MapType = llvm::DenseMap<const KeyType *, const ValueType *>;
  MapType ptrMap;

public:
  using const_iterator = typename MapType::const_iterator;

  ConstPointerMap() = default;

  // The insert() function here performs strong update
  void insert(const KeyType *key, const ValueType *val) {
    assert(val != nullptr);
    ptrMap[key] = val;
  }

  // Return nullptr if key not found
  const ValueType *lookup(const KeyType *key) const {
    auto itr = ptrMap.find(key);
    if (itr == ptrMap.end())
      return nullptr;
    else {
      assert(itr->second != nullptr);
      return itr->second;
    }
  }

  const_iterator begin() const { return ptrMap.begin(); }
  const_iterator end() const { return ptrMap.end(); }
};

} // namespace tpa

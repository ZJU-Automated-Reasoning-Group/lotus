//===-- Verification/Sifa/Storage/MapBasedStorage.h -----------------------===//
//
// Map-based LOI storage (ported from Ultimate Library-Sifa).
//
// Stores each location at most once; inserting twice is an error (as in Java).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_STORAGE_MAPBASEDSTORAGE_H
#define LOTUS_VERIFICATION_SIFA_STORAGE_MAPBASEDSTORAGE_H

#include "Verification/Sifa/Storage/ILoiStorage.h"

#include <stdexcept>
#include <unordered_map>

namespace lotus {
namespace sifa {

template <typename LocationT, typename StateT>
class MapBasedStorage final : public ILoiStorage<LocationT, StateT> {
public:
  void store(LocationT location, const StateT &state) override {
    auto res = map_.emplace(location, state);
    if (!res.second) {
      throw std::logic_error(
          "Tried to register predicate/state for LOI which already had one.");
    }
  }

  const std::unordered_map<LocationT, StateT> &getMap() const { return map_; }

  /// Ultimate-aligned: put default for each location not yet stored; return
  /// map.
  template <typename Container>
  const std::unordered_map<LocationT, StateT> &
  addDefaultsAndGetMap(const Container &locations, const StateT &defaultState) {
    for (const LocationT &loc : locations)
      map_.emplace(loc, defaultState);
    return map_;
  }

  StateT getSingletonOrDefault(const StateT &defaultState) const {
    if (map_.empty()) {
      return defaultState;
    }
    if (map_.size() == 1) {
      return map_.begin()->second;
    }
    throw std::logic_error("Expected single value but found multiple entries");
  }

private:
  std::unordered_map<LocationT, StateT> map_;
};

} // namespace sifa
} // namespace lotus

#include "llvm/IR/BasicBlock.h"

#include "Verification/Sifa/SifaSymAbs.h"
extern template class lotus::sifa::MapBasedStorage<const llvm::BasicBlock *,
                                                   bool>;
extern template class lotus::sifa::MapBasedStorage<const llvm::BasicBlock *,
                                                   lotus::sifa::SymAbsState>;

#endif // LOTUS_VERIFICATION_SIFA_STORAGE_MAPBASEDSTORAGE_H

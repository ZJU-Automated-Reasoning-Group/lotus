//===-- Verification/Sifa/Domain/NonrelationalState.h ---------------------===//
//
// State as map from variables to non-relational values (Ultimate-aligned).
//
// Ultimate's NonrelationalState<VALUE> implements IAbstractState: join/widen
// by merging maps (variable-wise join/widen); isBottom if any value is bottom.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_NONRELATIONALSTATE_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_NONRELATIONALSTATE_H

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Value.h"

#include "Verification/Sifa/Domain/INonrelationalValue.h"

#include <unordered_map>

namespace lotus {
namespace sifa {

/// Non-relational state: map from Value* to V. Ultimate
/// NonrelationalState<VALUE>. V must satisfy INonrelationalValue (join, widen,
/// isTop, isBottom).
template <typename V>
  requires INonrelationalValue<V>
class NonrelationalState {
public:
  using Value = V;
  using Map = std::unordered_map<const llvm::Value *, V>;

  NonrelationalState() = default;
  explicit NonrelationalState(Map m) : map_(std::move(m)) {}

  const Map &getMap() const { return map_; }
  Map &getMap() { return map_; }

  llvm::Optional<V> get(const llvm::Value *v) const {
    auto it = map_.find(v);
    if (it == map_.end())
      return llvm::None;
    return it->second;
  }
  void set(const llvm::Value *v, V val) { map_[v] = std::move(val); }

  NonrelationalState join(const NonrelationalState &other) const {
    NonrelationalState r;
    for (const auto &p : map_) {
      const auto *var = p.first;
      const auto &val = p.second;
      auto o = other.map_.find(var);
      if (o == other.map_.end()) {
        if (!val.isTop())
          r.map_[var] = val;
        continue;
      }
      V j = val.join(o->second);
      if (!j.isTop())
        r.map_[var] = std::move(j);
    }
    return r;
  }

  NonrelationalState widen(const NonrelationalState &other) const {
    NonrelationalState r;
    for (const auto &p : map_) {
      const auto *var = p.first;
      const auto &val = p.second;
      auto o = other.map_.find(var);
      if (o == other.map_.end()) {
        if (!val.isTop())
          r.map_[var] = val;
        continue;
      }
      V w = val.widen(o->second);
      if (!w.isTop())
        r.map_[var] = std::move(w);
    }
    return r;
  }

  bool isBottom() const {
    for (const auto &p : map_) {
      if (p.second.isBottom())
        return true;
    }
    return false;
  }

private:
  Map map_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_NONRELATIONALSTATE_H

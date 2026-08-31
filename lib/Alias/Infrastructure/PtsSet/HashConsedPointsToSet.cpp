#include "Alias/Infrastructure/PtsSet/HashConsedPointsToSet.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace lotus::alias {

HashConsedPointsToSetArena::HashConsedPointsToSetArena() { reset(); }

std::size_t
HashConsedPointsToSetArena::SetHash::operator()(const Set &set) const noexcept {
  std::size_t seed = set.size();
  for (ObjectID object : set)
    seed ^= std::hash<ObjectID>{}(object) + 0x9e3779b97f4a7c15ULL +
            (seed << 6U) + (seed >> 2U);
  return seed;
}

std::uint64_t HashConsedPointsToSetArena::commutativeKey(SetID lhs, SetID rhs) {
  if (rhs < lhs)
    std::swap(lhs, rhs);
  return orderedKey(lhs, rhs);
}

std::uint64_t HashConsedPointsToSetArena::orderedKey(SetID lhs, SetID rhs) {
  return (static_cast<std::uint64_t>(lhs) << 32U) | rhs;
}

void HashConsedPointsToSetArena::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  sets_.clear();
  setToId_.clear();
  singletonCache_.clear();
  unionCache_.clear();
  intersectionCache_.clear();
  differenceCache_.clear();
  stats_ = {};
  sets_.push_back(std::make_unique<const Set>());
  setToId_.emplace(Set{}, EmptySet);
  stats_.uniqueSets = 1;
}

HashConsedPointsToSetArena::SetID
HashConsedPointsToSetArena::internUnlocked(Set set) {
  auto existing = setToId_.find(set);
  if (existing != setToId_.end()) {
    ++stats_.internHits;
    return existing->second;
  }
  const SetID id = static_cast<SetID>(sets_.size());
  stats_.storedElements += set.size();
  sets_.push_back(std::make_unique<const Set>(set));
  setToId_.emplace(std::move(set), id);
  stats_.uniqueSets = sets_.size();
  return id;
}

HashConsedPointsToSetArena::SetID
HashConsedPointsToSetArena::intern(const Set &set) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.internRequests;
  return internUnlocked(set);
}

HashConsedPointsToSetArena::SetID
HashConsedPointsToSetArena::singleton(ObjectID object) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto existing = singletonCache_.find(object);
  if (existing != singletonCache_.end())
    return existing->second;
  ++stats_.internRequests;
  const SetID id = internUnlocked(Set{object});
  singletonCache_[object] = id;
  return id;
}

HashConsedPointsToSetArena::SetID HashConsedPointsToSetArena::unite(SetID lhs,
                                                                    SetID rhs) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(lhs < sets_.size() && rhs < sets_.size());
  ++stats_.unionRequests;
  if (lhs == EmptySet)
    return rhs;
  if (rhs == EmptySet || lhs == rhs)
    return lhs;
  const std::uint64_t key = commutativeKey(lhs, rhs);
  auto cached = unionCache_.find(key);
  if (cached != unionCache_.end()) {
    ++stats_.unionCacheHits;
    return cached->second;
  }
  Set result;
  std::set_union(sets_[lhs]->begin(), sets_[lhs]->end(), sets_[rhs]->begin(),
                 sets_[rhs]->end(), std::inserter(result, result.end()));
  ++stats_.internRequests;
  const SetID id = internUnlocked(std::move(result));
  unionCache_[key] = id;
  return id;
}

HashConsedPointsToSetArena::SetID
HashConsedPointsToSetArena::intersect(SetID lhs, SetID rhs) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(lhs < sets_.size() && rhs < sets_.size());
  ++stats_.intersectionRequests;
  if (lhs == EmptySet || rhs == EmptySet)
    return EmptySet;
  if (lhs == rhs)
    return lhs;
  const std::uint64_t key = commutativeKey(lhs, rhs);
  auto cached = intersectionCache_.find(key);
  if (cached != intersectionCache_.end()) {
    ++stats_.intersectionCacheHits;
    return cached->second;
  }
  Set result;
  std::set_intersection(sets_[lhs]->begin(), sets_[lhs]->end(),
                        sets_[rhs]->begin(), sets_[rhs]->end(),
                        std::inserter(result, result.end()));
  ++stats_.internRequests;
  const SetID id = internUnlocked(std::move(result));
  intersectionCache_[key] = id;
  return id;
}

HashConsedPointsToSetArena::SetID
HashConsedPointsToSetArena::difference(SetID lhs, SetID rhs) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(lhs < sets_.size() && rhs < sets_.size());
  ++stats_.differenceRequests;
  if (lhs == EmptySet || lhs == rhs)
    return EmptySet;
  if (rhs == EmptySet)
    return lhs;
  const std::uint64_t key = orderedKey(lhs, rhs);
  auto cached = differenceCache_.find(key);
  if (cached != differenceCache_.end()) {
    ++stats_.differenceCacheHits;
    return cached->second;
  }
  Set result;
  std::set_difference(sets_[lhs]->begin(), sets_[lhs]->end(),
                      sets_[rhs]->begin(), sets_[rhs]->end(),
                      std::inserter(result, result.end()));
  ++stats_.internRequests;
  const SetID id = internUnlocked(std::move(result));
  differenceCache_[key] = id;
  return id;
}

const HashConsedPointsToSetArena::Set &
HashConsedPointsToSetArena::get(SetID id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(id < sets_.size());
  return *sets_[id];
}

bool HashConsedPointsToSetArena::contains(SetID id, ObjectID object) const {
  const Set &set = get(id);
  return set.count(object) != 0;
}

bool HashConsedPointsToSetArena::intersects(SetID lhs, SetID rhs) const {
  std::lock_guard<std::mutex> lock(mutex_);
  assert(lhs < sets_.size() && rhs < sets_.size());
  const Set *small = sets_[lhs].get();
  const Set *large = sets_[rhs].get();
  if (large->size() < small->size())
    std::swap(small, large);
  return std::any_of(small->begin(), small->end(), [&](ObjectID object) {
    return large->count(object) != 0;
  });
}

std::size_t HashConsedPointsToSetArena::size(SetID id) const {
  return get(id).size();
}

HashConsedPointsToSetArena::Statistics
HashConsedPointsToSetArena::statistics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

HashConsedPointsToSet
HashConsedPointsToSet::unite(const HashConsedPointsToSet &other) const {
  assert(arena_ == other.arena_);
  return HashConsedPointsToSet(*arena_, arena_->unite(id_, other.id_));
}

HashConsedPointsToSet
HashConsedPointsToSet::intersect(const HashConsedPointsToSet &other) const {
  assert(arena_ == other.arena_);
  return HashConsedPointsToSet(*arena_, arena_->intersect(id_, other.id_));
}

HashConsedPointsToSet
HashConsedPointsToSet::difference(const HashConsedPointsToSet &other) const {
  assert(arena_ == other.arena_);
  return HashConsedPointsToSet(*arena_, arena_->difference(id_, other.id_));
}

bool HashConsedPointsToSet::intersects(
    const HashConsedPointsToSet &other) const {
  assert(arena_ == other.arena_);
  return arena_->intersects(id_, other.id_);
}

} // namespace lotus::alias

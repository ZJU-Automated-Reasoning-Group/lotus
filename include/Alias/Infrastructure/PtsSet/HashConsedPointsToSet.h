/**
 * @file HashConsedPointsToSet.h
 * @brief Immutable interned points-to sets with memoized set operations.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace lotus::alias {

enum class PointsToSetBackend { Mutable, HashConsed };

class HashConsedPointsToSetArena {
public:
  using ObjectID = std::uint32_t;
  using Set = std::set<ObjectID>;
  using SetID = std::uint32_t;
  static constexpr SetID EmptySet = 0;

  struct Statistics {
    std::size_t uniqueSets = 0;
    std::size_t internRequests = 0;
    std::size_t internHits = 0;
    std::size_t unionRequests = 0;
    std::size_t unionCacheHits = 0;
    std::size_t intersectionRequests = 0;
    std::size_t intersectionCacheHits = 0;
    std::size_t differenceRequests = 0;
    std::size_t differenceCacheHits = 0;
    std::size_t storedElements = 0;
  };

  HashConsedPointsToSetArena();
  HashConsedPointsToSetArena(const HashConsedPointsToSetArena &) = delete;
  HashConsedPointsToSetArena &
  operator=(const HashConsedPointsToSetArena &) = delete;

  void reset();

  SetID intern(const Set &set);
  SetID singleton(ObjectID object);
  SetID unite(SetID lhs, SetID rhs);
  SetID intersect(SetID lhs, SetID rhs);
  SetID difference(SetID lhs, SetID rhs);

  const Set &get(SetID id) const;
  bool contains(SetID id, ObjectID object) const;
  bool intersects(SetID lhs, SetID rhs) const;
  std::size_t size(SetID id) const;
  Statistics statistics() const;

private:
  struct SetHash {
    std::size_t operator()(const Set &set) const noexcept;
  };

  static std::uint64_t commutativeKey(SetID lhs, SetID rhs);
  static std::uint64_t orderedKey(SetID lhs, SetID rhs);
  SetID internUnlocked(Set set);

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<const Set>> sets_;
  std::unordered_map<Set, SetID, SetHash> setToId_;
  std::unordered_map<ObjectID, SetID> singletonCache_;
  std::unordered_map<std::uint64_t, SetID> unionCache_;
  std::unordered_map<std::uint64_t, SetID> intersectionCache_;
  std::unordered_map<std::uint64_t, SetID> differenceCache_;
  Statistics stats_;
};

class HashConsedPointsToSet {
public:
  using ObjectID = HashConsedPointsToSetArena::ObjectID;
  using SetID = HashConsedPointsToSetArena::SetID;
  using const_iterator = HashConsedPointsToSetArena::Set::const_iterator;

  explicit HashConsedPointsToSet(HashConsedPointsToSetArena &arena)
      : arena_(&arena), id_(HashConsedPointsToSetArena::EmptySet) {}
  HashConsedPointsToSet(HashConsedPointsToSetArena &arena, SetID id)
      : arena_(&arena), id_(id) {}

  static HashConsedPointsToSet singleton(HashConsedPointsToSetArena &arena,
                                         ObjectID object) {
    return HashConsedPointsToSet(arena, arena.singleton(object));
  }

  HashConsedPointsToSet unite(const HashConsedPointsToSet &other) const;
  HashConsedPointsToSet intersect(const HashConsedPointsToSet &other) const;
  HashConsedPointsToSet difference(const HashConsedPointsToSet &other) const;

  bool contains(ObjectID object) const { return arena_->contains(id_, object); }
  bool intersects(const HashConsedPointsToSet &other) const;
  bool empty() const { return id_ == HashConsedPointsToSetArena::EmptySet; }
  std::size_t size() const { return arena_->size(id_); }
  SetID id() const { return id_; }
  const HashConsedPointsToSetArena::Set &materialize() const {
    return arena_->get(id_);
  }
  const_iterator begin() const { return materialize().begin(); }
  const_iterator end() const { return materialize().end(); }

  bool operator==(const HashConsedPointsToSet &other) const {
    return arena_ == other.arena_ && id_ == other.id_;
  }
  bool operator!=(const HashConsedPointsToSet &other) const {
    return !(*this == other);
  }

private:
  HashConsedPointsToSetArena *arena_;
  SetID id_;
};

} // namespace lotus::alias

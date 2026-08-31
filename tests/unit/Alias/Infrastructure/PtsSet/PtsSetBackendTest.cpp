#include "Alias/Infrastructure/PtsSet/BloomBitsetPtsSet.h"
#include "Alias/Infrastructure/PtsSet/ChunkedSparseBitsetPtsSet.h"
#include "Alias/Infrastructure/PtsSet/HashConsedPointsToSet.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

template <typename SetT> std::vector<std::uint64_t> collect(const SetT &set) {
  std::vector<std::uint64_t> out;
  for (auto id : set)
    out.push_back(static_cast<std::uint64_t>(id));
  return out;
}

template <typename SetT> void expectCommonSetSemantics() {
  SetT lhs;
  SetT rhs;

  EXPECT_TRUE(lhs.isEmpty());
  EXPECT_TRUE(lhs.insert(1));
  EXPECT_FALSE(lhs.insert(1));
  EXPECT_TRUE(lhs.insert(63));
  EXPECT_TRUE(lhs.insert(64));
  EXPECT_TRUE(lhs.insert(1024));
  EXPECT_TRUE(lhs.insert(2049));

  EXPECT_TRUE(lhs.has(1));
  EXPECT_TRUE(lhs.has(63));
  EXPECT_TRUE(lhs.has(64));
  EXPECT_TRUE(lhs.has(1024));
  EXPECT_TRUE(lhs.has(2049));
  EXPECT_FALSE(lhs.has(2));
  EXPECT_EQ(lhs.getSize(), 5U);

  EXPECT_TRUE(rhs.insert(64));
  EXPECT_TRUE(rhs.insert(2049));
  EXPECT_TRUE(lhs.contains(rhs));
  EXPECT_TRUE(lhs.intersectWith(rhs));
  EXPECT_TRUE(lhs.intersects(rhs));

  EXPECT_TRUE(rhs.insert(4097));
  EXPECT_FALSE(lhs.contains(rhs));
  EXPECT_TRUE(lhs.unionWith(rhs));
  EXPECT_TRUE(lhs.contains(rhs));
  EXPECT_TRUE(lhs.has(4097));
  EXPECT_FALSE(lhs.unionWith(rhs));

  lhs.clear();
  EXPECT_TRUE(lhs.isEmpty());
  EXPECT_EQ(lhs.getSize(), 0U);
  EXPECT_FALSE(lhs.has(64));
  EXPECT_FALSE(lhs.intersectWith(rhs));
}

} // namespace

TEST(ChunkedSparseBitsetPtsSetTest, PreservesSetSemanticsAcrossChunks) {
  expectCommonSetSemantics<ChunkedSparseBitsetPtsSet>();
}

TEST(ChunkedSparseBitsetPtsSetTest, IteratesInAscendingOrder) {
  ChunkedSparseBitsetPtsSet set;
  set.insert(2050);
  set.insert(0);
  set.insert(1024);
  set.insert(65);
  set.insert(64);

  const std::vector<std::uint64_t> expected = {0, 64, 65, 1024, 2050};
  EXPECT_EQ(collect(set), expected);
}

TEST(BloomBitsetPtsSetTest, PreservesSetSemanticsAcrossWords) {
  expectCommonSetSemantics<BloomBitsetPtsSet<>>();
}

TEST(BloomBitsetPtsSetTest, IteratorDoesNotSkipAdjacentBits) {
  BloomBitsetPtsSet<> set;
  set.insert(0);
  set.insert(1);
  set.insert(2);
  set.insert(63);
  set.insert(64);
  set.insert(65);

  const std::vector<std::uint64_t> expected = {0, 1, 2, 63, 64, 65};
  EXPECT_EQ(collect(set), expected);
}

TEST(BloomBitsetPtsSetTest, BloomFilterDoesNotChangeExactIntersection) {
  BloomBitsetPtsSet<> lhs;
  BloomBitsetPtsSet<> rhs;

  lhs.insert(10);
  lhs.insert(5000);
  rhs.insert(11);
  rhs.insert(5001);

  EXPECT_FALSE(lhs.intersectWith(rhs));
  EXPECT_FALSE(lhs.contains(rhs));
}

TEST(HashConsedPointsToSetTest, InternsEqualSetsAndMemoizesOperations) {
  lotus::alias::HashConsedPointsToSetArena arena;
  const auto first = arena.intern({1, 2, 3});
  const auto duplicate = arena.intern({1, 2, 3});
  const auto second = arena.intern({3, 4});
  EXPECT_EQ(first, duplicate);

  const auto united = arena.unite(first, second);
  EXPECT_EQ(arena.get(united),
            lotus::alias::HashConsedPointsToSetArena::Set({1, 2, 3, 4}));
  EXPECT_EQ(arena.unite(second, first), united);

  const auto common = arena.intersect(first, second);
  EXPECT_EQ(arena.get(common),
            lotus::alias::HashConsedPointsToSetArena::Set({3}));
  EXPECT_EQ(arena.intersect(second, first), common);

  const auto remainder = arena.difference(first, second);
  EXPECT_EQ(arena.get(remainder),
            lotus::alias::HashConsedPointsToSetArena::Set({1, 2}));
  EXPECT_EQ(arena.difference(first, second), remainder);

  const auto stats = arena.statistics();
  EXPECT_GT(stats.internHits, 0u);
  EXPECT_GT(stats.unionCacheHits, 0u);
  EXPECT_GT(stats.intersectionCacheHits, 0u);
  EXPECT_GT(stats.differenceCacheHits, 0u);
}

TEST(HashConsedPointsToSetTest, ImmutableHandlesShareCanonicalIdentity) {
  lotus::alias::HashConsedPointsToSetArena arena;
  auto one = lotus::alias::HashConsedPointsToSet::singleton(arena, 1);
  auto two = lotus::alias::HashConsedPointsToSet::singleton(arena, 2);
  auto both = one.unite(two);
  auto reverse = two.unite(one);

  EXPECT_EQ(both, reverse);
  EXPECT_TRUE(both.contains(1));
  EXPECT_TRUE(both.contains(2));
  EXPECT_EQ(both.size(), 2u);
  EXPECT_EQ(both.difference(one), two);
  EXPECT_TRUE(both.intersects(one));
}

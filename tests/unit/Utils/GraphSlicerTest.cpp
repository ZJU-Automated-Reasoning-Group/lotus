#include <unordered_map>
#include <vector>

#include "Utils/ADT/GraphSlicer.h"
#include <gtest/gtest.h>

using namespace lotus;

// Simple graph: nodes 1..5, edges 1->2, 2->3, 3->4, 4->5, 2->4 (so backward
// from 4 = {1,2,3,4})
TEST(GraphSlicerTest, SliceBackward) {
  std::unordered_map<int, std::vector<int>> preds = {
      {1, {}}, {2, {1}}, {3, {2}}, {4, {2, 3}}, {5, {4}}};
  auto get_pred = [&](int n) { return preds[n]; };
  auto roots = slice_backward<int>(4, get_pred);
  EXPECT_TRUE(roots.count(1));
  EXPECT_TRUE(roots.count(2));
  EXPECT_TRUE(roots.count(3));
  EXPECT_TRUE(roots.count(4));
  EXPECT_EQ(roots.size(), 4u);
}

TEST(GraphSlicerTest, SliceForward) {
  std::unordered_map<int, std::vector<int>> succs = {
      {1, {2}}, {2, {3, 4}}, {3, {4}}, {4, {5}}, {5, {}}};
  auto get_succ = [&](int n) { return succs[n]; };
  auto reachable = slice_forward<int>(1, get_succ);
  EXPECT_TRUE(reachable.count(1));
  EXPECT_TRUE(reachable.count(2));
  EXPECT_TRUE(reachable.count(3));
  EXPECT_TRUE(reachable.count(4));
  EXPECT_TRUE(reachable.count(5));
  EXPECT_EQ(reachable.size(), 5u);
}

TEST(GraphSlicerTest, SliceBackwardMultipleRoots) {
  std::unordered_map<int, std::vector<int>> preds = {
      {1, {}}, {2, {1}}, {3, {2}}, {4, {3}}, {5, {4}}};
  auto get_pred = [&](int n) { return preds[n]; };
  auto roots = slice_backward<int>({4, 5}, get_pred);
  EXPECT_TRUE(roots.count(1));
  EXPECT_TRUE(roots.count(2));
  EXPECT_TRUE(roots.count(3));
  EXPECT_TRUE(roots.count(4));
  EXPECT_TRUE(roots.count(5));
  EXPECT_EQ(roots.size(), 5u);
}

TEST(GraphSlicerTest, SliceForwardCycle) {
  std::unordered_map<int, std::vector<int>> succs = {
      {1, {2}}, {2, {3}}, {3, {1}}, {4, {}}};
  auto get_succ = [&](int n) { return succs[n]; };
  auto reachable = slice_forward<int>(1, get_succ);
  EXPECT_TRUE(reachable.count(1));
  EXPECT_TRUE(reachable.count(2));
  EXPECT_TRUE(reachable.count(3));
  EXPECT_FALSE(reachable.count(4));
  EXPECT_EQ(reachable.size(), 3u);
}

TEST(GraphSlicerTest, PruneNodes) {
  std::unordered_set<int> nodes = {1, 2, 3, 4, 5};
  auto even = prune_nodes<int>(nodes, [](int n) { return n % 2 == 0; });
  EXPECT_EQ(even.size(), 2u);
  EXPECT_TRUE(even.count(2));
  EXPECT_TRUE(even.count(4));
}

TEST(GraphSlicerTest, PruneNodesIterator) {
  std::vector<int> nodes = {1, 2, 3, 4, 5};
  auto odd = prune_nodes<std::vector<int>::const_iterator, int>(
      nodes.begin(), nodes.end(), [](int n) { return n % 2 == 1; });
  EXPECT_EQ(odd.size(), 3u);
  EXPECT_TRUE(odd.count(1));
  EXPECT_TRUE(odd.count(3));
  EXPECT_TRUE(odd.count(5));
}

/**
 * @file AliasGraphTest.cpp
 * @brief Unit tests for AliasGraph (CC'18 must-alias data structure).
 */

#include "Alias/UnderApproxAA/AliasGraph.h"

#include <gtest/gtest.h>

using namespace UnderApprox;

namespace {

// Variable and field constants for tests
constexpr VarId Vx = 0, Vy = 1, Vz = 2, Va = 3, Vb = 4, Vu = 5, Vv = 6,
                Vw = 7;
constexpr FieldLabel Ff = 1, Fg = 2;

TEST(AliasGraphTest, AddVariableAndGetNode) {
  AliasGraph G;
  NodeId Nx = G.addVariable(Vx);
  EXPECT_NE(Nx, kNoNode);
  EXPECT_EQ(G.getNode(Vx), Nx);
  EXPECT_EQ(G.addVariable(Vx), Nx);
  NodeId Ny = G.addVariable(Vy);
  EXPECT_NE(Ny, Nx);
  EXPECT_EQ(G.getNode(Vy), Ny);
  EXPECT_EQ(G.numNodes(), 2u);
  EXPECT_EQ(G.numVariables(), 2u);
}

TEST(AliasGraphTest, MoveMerge) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  NodeId N = G.moveMerge(Vx, Vy);
  EXPECT_EQ(G.getNode(Vx), N);
  EXPECT_EQ(G.getNode(Vy), N);
  auto Vars = G.getNodeVars(N);
  EXPECT_EQ(Vars.size(), 2u);
}

TEST(AliasGraphTest, MoveMergeHandlesSelfLoop) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.storeEdge(Vx, Ff, Vx);
  G.storeEdge(Vy, Ff, Vy);

  NodeId Merged = G.moveMerge(Vx, Vy);
  EXPECT_EQ(G.getNode(Vx), Merged);
  EXPECT_EQ(G.getNode(Vy), Merged);
  EXPECT_EQ(G.getTarget(Merged, Ff), Merged);
}

TEST(AliasGraphTest, StoreEdge) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vz);
  G.storeEdge(Vx, Ff, Vz);
  NodeId Nx = G.getNode(Vx);
  NodeId Nz = G.getNode(Vz);
  EXPECT_EQ(G.getTarget(Nx, Ff), Nz);
}

TEST(AliasGraphTest, LoadEdge) {
  AliasGraph G;
  G.addVariable(Vy);
  G.addVariable(Vz);
  G.loadEdge(Vy, Fg, Vz);
  NodeId Ny = G.getNode(Vy);
  NodeId Nz = G.getNode(Vz);
  EXPECT_NE(Ny, Nz);
  EXPECT_EQ(G.getTarget(Ny, Fg), Nz);
  auto Vars = G.getNodeVars(Nz);
  EXPECT_EQ(Vars.size(), 1u);
  EXPECT_EQ(Vars[0], Vz);
}

TEST(AliasGraphTest, LoadEdgeUsesExistingFieldTarget) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vz);
  G.storeEdge(Vx, Ff, Vz);
  G.addVariable(Vy);
  G.loadEdge(Vx, Ff, Vy);

  EXPECT_EQ(G.getNode(Vy), G.getNode(Vz));
  NodeId Nx = G.getNode(Vx);
  EXPECT_EQ(G.getTarget(Nx, Ff), G.getNode(Vz));
}

TEST(AliasGraphTest, RenameVariable) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.moveMerge(Vx, Vy);
  // Rename Vx -> Va: Va joins the same alias class
  G.renameVariable(Vx, Va);
  EXPECT_EQ(G.getNode(Vx), kNoNode);
  EXPECT_EQ(G.getNode(Va), G.getNode(Vy));
  auto Vars = G.getNodeVars(G.getNode(Vy));
  EXPECT_EQ(Vars.size(), 2u);
  // Rename Vy -> Vb when Vb not in graph: Vb takes Vy's place
  G.renameVariable(Vy, Vb);
  EXPECT_EQ(G.getNode(Vy), kNoNode);
  EXPECT_EQ(G.getNode(Vb), G.getNode(Va));
}

TEST(AliasGraphTest, MustAliasAccessPath) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.moveMerge(Vx, Vy);
  G.addVariable(Vz);
  G.storeEdge(Vx, Ff, Vz);
  llvm::SmallVector<FieldLabel, 4> PathF;
  PathF.push_back(Ff);
  EXPECT_TRUE(G.mustAliasAccessPath(Vx, PathF, Vy, PathF));
  llvm::SmallVector<FieldLabel, 4> EmptyPath;
  EXPECT_TRUE(G.mustAliasAccessPath(Vx, EmptyPath, Vy, EmptyPath));
}

TEST(AliasGraphTest, StoreEdgeOverwritesWithoutMergingTargets) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.addVariable(Vz);
  G.storeEdge(Vx, Ff, Vy);
  G.storeEdge(Vx, Ff, Vz);

  NodeId Nx = G.getNode(Vx);
  EXPECT_EQ(G.getTarget(Nx, Ff), G.getNode(Vz));
  EXPECT_NE(G.getNode(Vy), G.getNode(Vz));
}

TEST(AliasGraphTest, StoreEdgeOverwriteCleansReverseEdges) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.addVariable(Vz);
  G.storeEdge(Vx, Ff, Vy);
  G.storeEdge(Vx, Ff, Vz);

  llvm::SmallVector<FieldLabel, 4> EmptyPath;
  llvm::SmallVector<std::pair<VarId, llvm::SmallVector<FieldLabel, 4>>, 8> Out;
  G.allAliases(Vy, EmptyPath, 2, Out);

  bool hasXF = false;
  for (const auto &P : Out) {
    if (P.first == Vx && P.second.size() == 1 && P.second[0] == Ff)
      hasXF = true;
  }
  EXPECT_FALSE(hasXF);
}

TEST(AliasGraphTest, Intersect) {
  AliasGraph G1, G2;
  G1.addVariable(Vx);
  G1.addVariable(Vy);
  G1.moveMerge(Vx, Vy);
  G1.addVariable(Vz);
  G1.storeEdge(Vx, Ff, Vz);

  G2.addVariable(Vx);
  G2.addVariable(Vy);
  G2.moveMerge(Vx, Vy);
  G2.addVariable(Vz);
  G2.storeEdge(Vx, Ff, Vz);

  AliasGraph R = AliasGraph::intersect(G1, G2);
  EXPECT_GE(R.numNodes(), 1u);
  EXPECT_TRUE(R.getNode(Vx) != kNoNode);
  EXPECT_EQ(R.getNode(Vx), R.getNode(Vy));
  llvm::SmallVector<FieldLabel, 4> EmptyPath;
  EXPECT_TRUE(R.mustAliasAccessPath(Vx, EmptyPath, Vy, EmptyPath));
}

TEST(AliasGraphTest, IntersectDisjointVars) {
  // G1: x -> f -> z; G2: y -> f -> z. Intersection has no var in common at root,
  // but (x,y) is not created; (z,z) is created. So result has at least node for z.
  AliasGraph G1, G2;
  G1.addVariable(Vx);
  G1.addVariable(Vz);
  G1.storeEdge(Vx, Ff, Vz);

  G2.addVariable(Vy);
  G2.addVariable(Vz);
  G2.storeEdge(Vy, Ff, Vz);

  AliasGraph R = AliasGraph::intersect(G1, G2);
  EXPECT_NE(R.getNode(Vz), kNoNode);
}

TEST(AliasGraphTest, IntersectKeepsMeaningfulEmptyNodeWithInEdges) {
  // G1: {x,u} -f-> y, z -g-> y
  AliasGraph G1;
  G1.addVariable(Vx);
  G1.addVariable(Vu);
  G1.moveMerge(Vx, Vu);
  G1.addVariable(Vy);
  G1.addVariable(Vz);
  G1.storeEdge(Vx, Ff, Vy);
  G1.storeEdge(Vz, Fg, Vy);

  // G2: {x,w} -f-> v, z -g-> v
  AliasGraph G2;
  G2.addVariable(Vx);
  G2.addVariable(Vw);
  G2.moveMerge(Vx, Vw);
  G2.addVariable(Vv);
  G2.addVariable(Vz);
  G2.storeEdge(Vx, Ff, Vv);
  G2.storeEdge(Vz, Fg, Vv);

  AliasGraph R = AliasGraph::intersect(G1, G2);
  NodeId Nx = R.getNode(Vx);
  NodeId Nz = R.getNode(Vz);
  ASSERT_NE(Nx, kNoNode);
  ASSERT_NE(Nz, kNoNode);

  NodeId TFromX = R.getTarget(Nx, Ff);
  NodeId TFromZ = R.getTarget(Nz, Fg);
  ASSERT_NE(TFromX, kNoNode);
  ASSERT_EQ(TFromX, TFromZ);
  EXPECT_TRUE(R.getNodeVars(TFromX).empty());
}

TEST(AliasGraphTest, GcRemovesIsolatedSingleVar) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.moveMerge(Vx, Vy);
  NodeId Nz = G.addVariable(Vz);
  EXPECT_EQ(G.getNode(Vz), Nz);
  G.gc();
  EXPECT_EQ(G.getNode(Vx), G.getNode(Vy));
  // Single-variable node with no in/out edges is eliminated (paper §4.1)
  EXPECT_EQ(G.getNode(Vz), kNoNode);
}

TEST(AliasGraphTest, GcRunsToFixpoint) {
  // Build a graph where removing an empty node makes a single-var node dead.
  AliasGraph G1, G2;
  G1.addVariable(Vx);
  G1.addVariable(Vy);
  G1.storeEdge(Vx, Ff, Vy);

  G2.addVariable(Vx);
  G2.addVariable(Vz);
  G2.storeEdge(Vx, Ff, Vz);

  AliasGraph R = AliasGraph::intersect(G1, G2);
  ASSERT_NE(R.getNode(Vx), kNoNode);
  R.gc();
  EXPECT_EQ(R.getNode(Vx), kNoNode);
  EXPECT_EQ(R.numNodes(), 0u);
}

TEST(AliasGraphTest, AllAliases) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.moveMerge(Vx, Vy);
  G.addVariable(Vz);
  G.storeEdge(Vx, Ff, Vz);
  llvm::SmallVector<FieldLabel, 4> PathF;
  PathF.push_back(Ff);
  llvm::SmallVector<std::pair<VarId, llvm::SmallVector<FieldLabel, 4>>, 8> Out;
  G.allAliases(Vx, PathF, 2, Out);
  EXPECT_GE(Out.size(), 1u);
  // Vx.f and Vz (path []) both reach the same node, so (Vz, []) is an alias of (Vx, [f])
  bool hasZ = false;
  for (const auto &P : Out) {
    if (P.first == Vz && P.second.empty())
      hasZ = true;
  }
  EXPECT_TRUE(hasZ);
}

TEST(AliasGraphTest, AllAliasesHandlesCycleUpToBound) {
  AliasGraph G;
  G.addVariable(Vx);
  G.storeEdge(Vx, Ff, Vx);

  llvm::SmallVector<FieldLabel, 4> EmptyPath;
  llvm::SmallVector<std::pair<VarId, llvm::SmallVector<FieldLabel, 4>>, 8> Out;
  G.allAliases(Vx, EmptyPath, 3, Out);

  bool hasLen0 = false, hasLen1 = false, hasLen2 = false, hasLen3 = false;
  for (const auto &P : Out) {
    if (P.first != Vx)
      continue;
    if (P.second.size() == 0)
      hasLen0 = true;
    if (P.second.size() == 1 && P.second[0] == Ff)
      hasLen1 = true;
    if (P.second.size() == 2 && P.second[0] == Ff && P.second[1] == Ff)
      hasLen2 = true;
    if (P.second.size() == 3 && P.second[0] == Ff && P.second[1] == Ff &&
        P.second[2] == Ff)
      hasLen3 = true;
  }
  EXPECT_TRUE(hasLen0);
  EXPECT_TRUE(hasLen1);
  EXPECT_TRUE(hasLen2);
  EXPECT_TRUE(hasLen3);
}

TEST(AliasGraphTest, CopyAndAssign) {
  AliasGraph G;
  G.addVariable(Vx);
  G.addVariable(Vy);
  G.moveMerge(Vx, Vy);
  AliasGraph G2(G);
  EXPECT_EQ(G2.getNode(Vx), G2.getNode(Vy));
  AliasGraph G3;
  G3 = G;
  EXPECT_EQ(G3.getNode(Vx), G3.getNode(Vy));
}

} // namespace

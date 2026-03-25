/// @file GraphAnalysis.h
/// @brief Graph analysis utilities for ICFG and CFG.
///
/// Provides functions for finding back edges, computing shortest paths,
/// and reachability analysis on control flow graphs.

#ifndef LT_GRAPH_ANALYSIS_H
#define LT_GRAPH_ANALYSIS_H

#include "IR/ICFG/ICFG.h"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>

// typedef std::pair<llvm::BasicBlock*, llvm::BasicBlock*> BBEdgePair;
using BBEdgePair = std::pair<llvm::BasicBlock *, llvm::BasicBlock *>;

/// @brief Finds all intraprocedural back edges in a function.
/// @param func Function to analyze.
/// @param res Output set of back edges (tail, header).
void findFunctionBackedgesIntra(llvm::Function *func,
                                std::set<BBEdgePair> &res);

/// @brief Finds all back edges reachable from a basic block.
/// @param sourceBB Starting basic block.
/// @param res Output set of back edges.
void findBackedgesFromBasicBlock(llvm::BasicBlock *sourceBB,
                                 std::set<BBEdgePair> &res);

/// @brief Finds all intraprocedural back edges in the ICFG.
/// @param icfg The ICFG to analyze.
/// @param func Function to analyze.
/// @param res Output set of back edges.
void findFunctionBackedgesIntraICFG(ICFG *icfg, const llvm::Function *func,
                                    std::set<ICFGEdge *> &res);

/// @brief Finds all interprocedural back edges (recursive calls) in the ICFG.
/// @param icfg The ICFG to analyze.
/// @param func Function to analyze.
/// @param res Output set of back edges.
void findFunctionBackedgesInterICFG(ICFG *icfg, const llvm::Function *func,
                                    std::set<ICFGEdge *> &res);

/// @brief Computes shortest distances from a source basic block.
/// @param sourceBB Starting basic block.
/// @return Map of basic block to distance.
std::map<llvm::BasicBlock *, uint64_t>
calculateDistanceMapIntra(llvm::BasicBlock *sourceBB);

/// @brief Computes shortest distances from a source basic block, ignoring back
/// edges.
/// @param sourceBB Starting basic block.
/// @param backEdges Set of back edges to ignore.
/// @return Map of basic block to distance.
std::map<llvm::BasicBlock *, uint64_t>
calculateDistanceMapIntra(llvm::BasicBlock *sourceBB,
                          const std::set<BBEdgePair> &backEdges);

/// @brief Computes shortest distances in an acyclic ICFG.
/// @param icfg The ICFG to analyze.
/// @param sourceBB Starting node.
/// @return Map of ICFG node to distance.
std::map<ICFGNode *, uint64_t>
calculateDistanceMapInterICFG(ICFG *icfg, ICFGNode *sourceBB);

/// @brief Computes shortest distances in an acyclic ICFG using provided map.
/// @param icfg The ICFG to analyze.
/// @param sourceBB Starting node.
/// @param distanceMap Output map to populate with distances.
void calculateDistanceMapInterICFGWithDistanceMap(
    ICFG *icfg, ICFGNode *sourceBB,
    std::map<ICFGNode *, uint64_t> &distanceMap);

/// @brief Computes shortest path between two basic blocks.
/// @param sourceBB Source basic block.
/// @param destBB Destination basic block.
/// @param path Output vector containing the path.
/// @return True if a path exists.
bool calculateShortestPathIntra(llvm::BasicBlock *sourceBB,
                                llvm::BasicBlock *destBB,
                                std::vector<llvm::BasicBlock *> &path);

/// @brief Checks if one basic block can reach another.
/// @param from Source basic block.
/// @param to Destination basic block.
/// @param DT Dominator tree (optional).
/// @param LI Loop info (optional).
/// @param iterCount Output iteration count.
/// @return True if reachable.
bool isReachableFrom(llvm::BasicBlock *from, llvm::BasicBlock *to,
                     const llvm::DominatorTree *DT, const llvm::LoopInfo *LI,
                     int &iterCount);

#endif // LT_GRAPH_ANALYSIS_H
//===-- Nisse.h ----------------------------------------------*- C++ -*-===//
// Copyright (C) 2023 Leon Frenot
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declarations used to apply the edge profiling
/// algorithm.
//
//===----------------------------------------------------------------------===//

#ifndef NISSE_H
#define NISSE_H

#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"

#include <fstream>
#include <map>

namespace nisse {

/// \brief Pointer to a Basic Block
using BlockPtr = llvm::BasicBlock *;

/// \struct Edge
///
/// \brief Representation of a CFG edge connecting two basic blocks.
///
struct Edge {
private:
  BlockPtr origin; ///< The origin of the edge.
  BlockPtr dest;   ///< The destination of the edge.
  int index;       ///< Index of the edge.
  int weight;      ///< An expectation of how often this edge will be executed.

  bool flagSESE; ///< Flag to use if the edge is instrumenting a SESE region.

  llvm::Value
      *indVar; ///< User defined induction variable (for well-founded loops).
  llvm::Value
      *initValue;   ///< User defined initial value (for well-founded loops).
  double incrValue; ///< User defined increment value (for well-founded loops).
  llvm::SmallVector<BlockPtr>
      exitBlocks; ///< List of exit blocks (for well-founded loops).

  /// \brief Instruments the edge with an increment counter.
  /// \param i The index of the array to increment.
  /// \param inst The instruction to the counter-array.
  void insertSimpleIncrFn(int i, llvm::Value *inst);

  /// \brief Instruments the edge with a well-founded loop counter.
  /// \param i The index of the array to increment.
  /// \param inst The instruction to the counter-array.
  void insertSESEIncrFn(int i, llvm::Value *inst);

  /// \brief Casts a value to i32.
  /// \param inst The value to cast.
  /// \param builder The builder where to insert the cast.
  /// \return The casted value.
  llvm::Value *createInt32Cast(llvm::Value *inst, llvm::IRBuilder<> &builder);

  /// \brief Casts a value to i64.
  /// \param inst The value to cast.
  /// \param builder The builder where to insert the cast.
  /// \return The casted value.
  llvm::Value *createInt64Cast(llvm::Value *inst, llvm::IRBuilder<> &builder);

  /// \brief Computes the hook to insert the KS counter.
  /// If the source block terminates with an absolute jump, the counter is
  /// placed at the end of that block. If not, it is placed at the start of the
  /// destination block. If the hooked block is the entry block, the counter
  /// will always be placed at the end of the block.
  /// \return The pointer to the hook for the counter.
  llvm::Instruction *getInstrumentationPoint() const;

public:
  /// \brief Default constructor for Edge.
  /// \param origin The origin of the edge.
  /// \param dest The destination of the edge.
  /// \param index An index for the edge.
  /// \param weight (optional) An expectation of how often the edge will be
  /// executed.
  Edge(BlockPtr origin, BlockPtr dest, int index, int weight = 1)
      : origin(origin), dest(dest), index(index), weight(weight),
        flagSESE(false) {};

  /// \brief Getter for the destination of the edge.
  /// \return The destination of the edge.
  BlockPtr getOrigin() const;

  /// \brief Getter for the destination of the edge.
  /// \return The destination of the edge.
  BlockPtr getDest() const;

  /// \brief Sets the variables for a well founded loop's back edge.
  /// \param indVar The induction variable.
  /// \param initValue The induction variable's original value.
  /// \param incrVal The induction variable's increment.
  /// \param exitBlocks The loop's exit blocks.
  /// \param weight The edge's new weight.
  void setSESE(llvm::Value *indVar, llvm::Value *initValue,
               const llvm::APInt *incrVal,
               llvm::SmallVector<BlockPtr> &exitBlocks, int weight = 0);

  /// \brief Sets the variables for a well founded loop's back edge.
  bool isSESE();

  /// \brief Instruments the edge.
  /// \param i The index of the array to increment.
  /// \param inst The instruction to the counter-array.
  void insertIncrFn(int i, llvm::Value *inst);

  /// \brief Getter for the edge's index.
  /// \return the edge's index.
  int getIndex() const;

  /// \brief Getter for the name of the edge.
  /// If there is no user-defined name, will return the edge's index.
  /// \return Returns the name of the edge.
  std::string getName() const;

  /// \brief Checks if two edges have the same origins and destinations.
  /// \param e Edge to compare the current edge to.
  /// \return true if the current edge and e have the same origins and
  /// destinations.
  bool operator==(const Edge &e) const;

  /// \brief Compares the weight of two edges.
  /// \param e Edge to compare the current edge to.
  /// \return true if the current edge has lower weight than e.
  bool operator<(const Edge &e) const;

  /// \brief Compares the weight of two edges.
  /// \param e Edge to compare the current edge to.
  /// \return true if the current edge has higher weight than e.
  bool operator>(const Edge &e) const;

  /// \brief Compares the weight of two edges.
  /// \param a First edge to compare.
  /// \param b Second edge to compare.
  /// \return true if a has higher weight than b.
  static bool compareWeights(const Edge &a, const Edge &b);

  /// \brief Prints the edge name, the name of its origin, and the name of its
  /// destination.
  /// \param os Output stream
  /// \param e Edge to print
  /// \return the output stream
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const Edge &e);

  /// \brief Prints the number of the edge's origin and the number of its
  /// destination.
  /// \param os Output stream
  /// \param e Edge to print
  /// \return the output stream
  friend std::ostream &operator<<(std::ostream &os, const Edge &e);
};

/// \struct UnionFind
///
/// \brief Implements union-find structure for Kruskal's Maximal Spanning Tree
/// algorithm
/// \see NisseAnalysis
struct UnionFind {
private:
  int cnt;                     ///< Number of elements in the structure.
  std::map<void *, void *> id; ///< Maps elements to their root.
  std::map<void *, int> sz;    ///< Maps roots to the size of their group.

public:
  /// \brief Initialises a new element in the structure.
  /// \param x Element to initialise.
  void init(void *x);

  /// \brief Finds the root of x, and collapses the path from x to its root.
  /// \param x Element to find the root of.
  /// \return The root of x.
  void *find(void *x);

  /// \brief Merges the sets of x and y, updates the weights, and makes the
  /// smaller root point to larger one. \param x First element to merge. \param
  /// y Second element to merge.
  void merge(void *x, void *y);

  /// \brief Checks if x and y belong to the same set.
  /// \param x First element to check.
  /// \param y Second element to check.
  /// \return true is x and y belong to the same set.
  bool connected(void *x, void *y);
};

struct AnalysisUtil {
public:
  /// \brief Find the return block of a function.
  /// \param F The function to find the return block of.
  /// \return The return block.
  static BlockPtr findReturnBlock(llvm::Function &F);

  /// \brief Returns the number corresponding to the block name give in argument
  /// (bbX -> X, bb -> 0)
  /// \param s String to get the corresponding number of
  /// \return the corresponding number
  static std::string removebb(const std::string &s);

  /// \brief Generates the set of edges of a function's CFG.
  /// \param F The function to compute the edges of.
  /// \return a vector containing the edges of F.
  static std::multiset<Edge> generateEdges(llvm::Function &F);

  /// \brief Generates the maximum spanning tree of a set of F's edges.
  /// \param F The function the edges belong to.
  /// \param edges The set of edges to generate the maximum spanning tree of.
  /// \return A pair of the set of edges in the spanning tree and the set of
  /// edges in its complementary.
  static std::pair<std::multiset<Edge>, std::multiset<Edge>>
  generateSTrev(llvm::Function &F, std::multiset<Edge> &edges);

  /// \brief Saves the CFG, the Spanning Tree and the instrumented edges to a
  /// file.
  /// \param F The function the edges belong to.
  /// \param edges The set of edges to generate the maximum spanning tree of.
  /// \param STrev the set of edges in the spanning tree and the set of
  /// edges in its complementary.
  static void
  printGraph(llvm::Function &F, std::multiset<Edge> &edges,
             std::pair<std::multiset<Edge>, std::multiset<Edge>> &STrev);
};

/// \struct NisseAnalysis
///
/// \brief Computes the maximum spanning tree of a function's CFG
/// \see Edge, UnionFind
struct NisseAnalysis : public llvm::AnalysisInfoMixin<NisseAnalysis> {

private:
  llvm::ScalarEvolution *SE;
  llvm::DominatorTree DT;
  llvm::PostDominatorTree PDT;
  llvm::LoopInfo LI;

  void initFunctionInfo(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);

  bool IsSESERegion(const BlockPtr &B1, const BlockPtr &B2);

  /// \brief Identifies if a PHI node defines a well founded induction variable
  /// (It is modified by a constant value each iteration of the loop). It also
  /// modifies the corresponding edge in edges.
  /// \param SE The function's scalar evolution.
  /// \param edges The CFG's edges.
  /// \param PHI The PHI node to analyse.
  /// \param incomingBlock The loop's incoming block.
  /// \param backBlock The outgoing block of the loop's back edge.
  /// \param backEdge The loop's back edge.
  /// \param exitBlocks The loop's exit blocks.
  /// \return true if the variable is a well founded induction variable.
  bool identifyInductionVariable(llvm::ScalarEvolution &SE,
                                 std::multiset<Edge> &edges, llvm::PHINode *PHI,
                                 BlockPtr incomingBlock, BlockPtr backBlock,
                                 Edge &backEdge,
                                 llvm::SmallVector<BlockPtr> &exitBlocks);

  /// \brief Identifies if a PHI node defines a well founded branch variable (It
  /// is modified by a constant value each time the loop goes through a set of
  /// control-equivalent blocks, and is not modified otherwise). It also
  /// modifies the corresponding edge in edges.
  /// \param SE The function's scalar evolution.
  /// \param edges The CFG's edges.
  /// \param PHI The PHI node to analyse.
  /// \param incomingBlock The loop's incoming block.
  /// \param backBlock The outgoing block of the loop's back edge.
  /// \param exitBlocks The loop's exit blocks.
  /// \param DT The function's dominator tree.
  /// \return true if the variable is a well founded branch variable.
  bool identifyBranchVariable(llvm::ScalarEvolution &SE,
                              std::multiset<Edge> &edges, llvm::PHINode *PHI,
                              BlockPtr incomingBlock, BlockPtr backBlock,
                              llvm::SmallVector<BlockPtr> &exitBlocks);

  /// \brief Identifies well founded edges for Nisse instrumentation.
  /// \param L Loop to instrument.
  /// \param SE The function's scalar evolution.
  /// \param edges The CFG's edges.
  void identifyWellFoundedEdges(llvm::Loop *L, llvm::ScalarEvolution &SE,
                                std::multiset<Edge> &edges);

public:
  /// \brief The return type of the analysis pass.
  using Result =
      std::tuple<std::multiset<Edge>, std::multiset<Edge>, std::multiset<Edge>>;

  /// \brief A special type used by analysis passes to provide an address that
  /// identifies that particular analysis pass type.
  static llvm::AnalysisKey Key;

  /// \brief The analysis pass' run function.
  /// \param F The function to analyse.
  /// \param FAM The current FunctionAnalysisManager.
  /// \return A triple with the set of edges in F, a maximum spanning tree, and
  /// its complementary.
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
};

/// \struct NissePass
///
/// \brief Instruments a function for edge instrumentation.
struct NissePass : public llvm::PassInfoMixin<NissePass> {
private:
  llvm::GlobalVariable *CounterArray = nullptr;
  llvm::GlobalVariable *IndexArray = nullptr;
  std::map<std::string, int> FunctionSize;
  int NumEdges = 0;
  int Offset = 0;
  std::ofstream outfile;

protected:
  /// \brief Inserts the initialization code, which creates a
  /// 0-initialized array of ints of size size.
  /// \param F The function to instrument.
  /// \param reverseSTEdges Set of edges that will be instrumented.
  /// \return The instruction pointer to the assignment of the array.
  std::pair<llvm::Value *, llvm::Value *>
  insertEntryFn(llvm::Function &F, std::multiset<Edge> &reverseSTEdges);

  /// \brief Inserts a call to the function that prints the results of the
  /// counter.
  /// \param F The function begin instrumented.
  /// \param counterInst The instruction pointer to the counter array.
  /// \param indexInst The instruction pointer to the index array.
  /// \param size The size of the counter and index arrays.
  void insertExitFn(llvm::Module &M, llvm::Function &F,
                    llvm::Value *counterInst, llvm::Value *indexInst, int size);

public:
  /// \brief The transformation pass' run function. Instruments the function
  /// given as argument for KS edge instrumentation.
  /// \param F The function to transform.
  /// \param FAM The current FunctionAnalysisManager.
  llvm::PreservedAnalyses run(llvm::Module &F,
                              llvm::ModuleAnalysisManager &FAM);
};

/// \brief Computes the maximum spanning tree of a function's CFG
struct KSAnalysis : public llvm::AnalysisInfoMixin<KSAnalysis> {

  /// \brief The return type of the analysis pass.
  using Result =
      std::tuple<std::multiset<Edge>, std::multiset<Edge>, std::multiset<Edge>>;

  /// \brief A special type used by analysis passes to provide an address that
  /// identifies that particular analysis pass type.
  static llvm::AnalysisKey Key;

  /// \brief The analysis pass' run function.
  /// \param F The function to analyse.
  /// \param FAM The current FunctionAnalysisManager.
  /// \return A triple with the set of edges in F, a maximum spanning tree, and
  /// its complementary.
  Result run(llvm::Function &F, llvm::FunctionAnalysisManager &FAM);
};

/// \brief Instruments a function for KS edge instrumentation.
struct KSPass : public NissePass {
private:
  llvm::GlobalVariable *CounterArray = nullptr;
  llvm::GlobalVariable *IndexArray = nullptr;
  std::map<std::string, int> FunctionSize;
  int NumEdges = 0;
  int Offset = 0;
  std::ofstream outfile;

public:
  /// \brief The transformation pass' run function. Instruments the function
  /// given as argument for KS edge instrumentation.
  /// \param F The function to transform.
  /// \param FAM The current FunctionAnalysisManager.
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);
};

} // namespace nisse

#endif

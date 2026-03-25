/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef DYCKAA_DYCKCALLGRAPH_H
#define DYCKAA_DYCKCALLGRAPH_H

#include "Alias/DyckAA/DyckCallGraphNode.h"
#include "Utils/ADT/MapIterators.h"

#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

typedef std::map<Function *, DyckCallGraphNode *> FunctionMapTy;

class DyckCallGraph {
private:
  /// function -> call graph node
  FunctionMapTy FunctionMap;

  /// This node has edges to all external functions and those internal
  /// functions that have their address taken.
  DyckCallGraphNode *ExternalCallingNode;

public:
  DyckCallGraph();

  ~DyckCallGraph();

  FunctionMapTy::iterator begin() { return FunctionMap.begin(); }

  FunctionMapTy::iterator end() { return FunctionMap.end(); }

  value_iterator<FunctionMapTy::iterator> nodes_begin() {
    return {FunctionMap.begin()};
  }

  value_iterator<FunctionMapTy::iterator> nodes_end() {
    return {FunctionMap.end()};
  }

  value_iterator<FunctionMapTy::const_iterator> nodes_begin() const {
    return {FunctionMap.begin()};
  }

  value_iterator<FunctionMapTy::const_iterator> nodes_end() const {
    return {FunctionMap.end()};
  }

  size_t size() const { return FunctionMap.size(); }

  DyckCallGraphNode *getOrInsertFunction(Function *);

  DyckCallGraphNode *getFunction(Function *) const;

  void dotCallGraph(const std::string &ModuleIdentifier);

  void printFunctionPointersInformation(const std::string &ModuleIdentifier);
};

//===----------------------------------------------------------------------===//
// GraphTraits specializations for DyckCG so that it can be treated as
// graphs by generic graph algorithms.
//
namespace llvm {
template <> struct GraphTraits<DyckCallGraphNode *> {
  using NodeRef = DyckCallGraphNode *;
  using ChildIteratorType =
      pair_value_iterator<CallRecordVecTy::iterator, DyckCallGraphNode *>;

  static NodeRef getEntryNode(DyckCallGraphNode *CGN) { return CGN; }

  static ChildIteratorType child_begin(NodeRef N) { return N->child_begin(); }

  static ChildIteratorType child_end(NodeRef N) { return N->child_end(); }
};

template <> struct GraphTraits<const DyckCallGraphNode *> {
  using NodeRef = const DyckCallGraphNode *;
  using EdgeRef = const CallRecordTy &;
  using ChildIteratorType =
      pair_value_iterator<CallRecordVecTy::const_iterator, DyckCallGraphNode *>;
  using ChildEdgeIteratorType = CallRecordVecTy::const_iterator;

  static NodeRef getEntryNode(const DyckCallGraphNode *CGN) { return CGN; }

  static ChildIteratorType child_begin(NodeRef N) { return N->child_begin(); }

  static ChildIteratorType child_end(NodeRef N) { return N->child_end(); }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->child_edge_begin();
  }

  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->child_edge_end();
  }

  static NodeRef edge_dest(EdgeRef E) { return E.second; }
};

template <>
struct GraphTraits<DyckCallGraph *> : public GraphTraits<DyckCallGraphNode *> {
  static NodeRef getEntryNode(DyckCallGraph *CGN) {
    return CGN->getFunction(nullptr);
  }

  using nodes_iterator = value_iterator<FunctionMapTy::iterator>;

  static nodes_iterator nodes_begin(DyckCallGraph *CG) {
    return CG->nodes_begin();
  }

  static nodes_iterator nodes_end(DyckCallGraph *CG) { return CG->nodes_end(); }
};

template <>
struct GraphTraits<const DyckCallGraph *>
    : public GraphTraits<const DyckCallGraphNode *> {
  static NodeRef getEntryNode(const DyckCallGraph *CGN) {
    return CGN->getFunction(nullptr);
  }

  using nodes_iterator = value_iterator<FunctionMapTy::const_iterator>;

  static nodes_iterator nodes_begin(const DyckCallGraph *CG) {
    return CG->nodes_begin();
  }

  static nodes_iterator nodes_end(const DyckCallGraph *CG) {
    return CG->nodes_end();
  }
};
} // namespace llvm

#endif // DYCKAA_DYCKCALLGRAPH_H

#pragma once

#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <iostream>
#include <set>
#include <unordered_set>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <z3++.h>

namespace symabs_ai {
/**
 * A program fragment for which an abstract transformer can be computed.
 *
 * Location graph
 * ==============
 * Control Flow Graphs typically found in static program analysis have effects
 * associated either with nodes or edges. SSA form used in LLVM makes things
 * slightly more complicated: The computation happens mostly in nodes (basic
 * blocks) but phi instructions are evaluated according to the incoming edge.
 *
 * To define fragments, we will conceptually operate on a CFG with computation
 * happening on edges. This CFG is defined as:
 *  1. The set of locations is equal to the set of basic blocks of the LLVM IR
 *     *plus an additional location* Fragment::EXIT.
 *  2. The set of edges is equal to all the edges in the original basic block
 *     *plus an additional edge* to Fragment::EXIT from every location
 *     corresponding to a basic block with no successors.
 *
 * Therefore, Fragment::EXIT is the only location with no outgoing edges.
 *
 * By a *state at a location* we will mean the program state after executing
 * all the phi instructions in the basic block but before executing any non-phi
 * instruction. Fragment::EXIT behaves like an empty block.
 *
 * Thus, the computational effect of an edge between A and B corresponds to
 * evaluating all non-phi instructions in A and all phis in B.
 *
 * Example
 * -------
 * If a program's IR has only one basic block called S, we will consider this
 * program to have two locations: S and Fragment::EXIT. There is only one edge
 * and it's between S and Fragment::EXIT. The effect of this edge corresponds
 * to executing all the instructions in the basic block S (note that in LLVM
 * the entry block cannot contain any phis).
 *
 * What is a fragment?
 * ===================
 * Fragment is a acyclic subgraph of the location graph defined above, uniquely
 * determined by a nonempty set of edges E.
 *
 * A location that has outgoing edges in the set E but no incoming edges in E
 * will be called a *starting location*. Similarly, a location with incoming
 * edges in E but no outgoing edges will be called an *ending location*. You
 * can check whether a location belongs to one of these classes by calling
 * isStart() or isEnd().
 *
 * Each fragment can be thought to represent a set of computations that start
 * in some state in one of the starting locations and end in one of the ending
 * locations. As a special case, the fragment might include non-phi
 * instructions of its ending location. Use includesEndBody() to check whether
 * this is the case.
 *
 * \todo document how to get abstract transformers for a Fragment
 */

// TODO outdated comments

class Fragment {
public:
  typedef std::pair<llvm::BasicBlock *, llvm::BasicBlock *> edge;

  // noncopyable but moveable
  Fragment(const Fragment &) = delete;
  Fragment &operator=(const Fragment &) = delete;
  Fragment(Fragment &&) = default;
  Fragment &operator=(Fragment &&) = delete;

private:
  const FunctionContext &FunctionContext_;
  std::set<edge> Edges_;
  std::set<llvm::BasicBlock *> Locations_;
  llvm::BasicBlock *Start_;
  llvm::BasicBlock *End_;
  bool IncludesEndBody_;

  bool hasLoops();

public:
  /**
   * The exit location of the CFG.
   *
   * Every block that has no outgoing edges is implicitly assumed to have
   * and edge to this location.
   */
  static llvm::BasicBlock *EXIT;

  template <typename T>
  Fragment(const FunctionContext &fctx, llvm::BasicBlock *start,
           llvm::BasicBlock *end, const T &edges,
           bool includes_end_body = false)
      : FunctionContext_(fctx), Edges_(edges.begin(), edges.end()),
        Start_(start), End_(end), IncludesEndBody_(includes_end_body) {
    assert(start == end || !hasLoops());

    Locations_.insert(start);
    Locations_.insert(end);
    for (edge e : Edges_) {
      Locations_.insert(e.first);
      Locations_.insert(e.second);
    }
  }

  llvm::BasicBlock *getStart() const { return Start_; }
  llvm::BasicBlock *getEnd() const { return End_; }

  /**
   * The set of all edges in this Fragment.
   */
  const std::set<edge> &edges() const { return Edges_; }

  /**
   * Ranges over all the non-phi instructions of an edge.
   *
   * An edge e represents all non-phi instructions from e.first and all phi
   * instructions from e.second. This returns the former.
   */
  llvm::iterator_range<llvm::BasicBlock::iterator>
  edgeNonPhis(const edge &e) const {
    using namespace llvm;
    assert(edges().find(e) != edges().end());
    BasicBlock::iterator itr = e.first->begin();

    // Find first non-phi. There is always one since a terminator
    // instruction is not a phi.
    while (isa<PHINode>(*itr))
      ++itr;

    return iterator_range<BasicBlock::iterator>(itr, e.first->end());
  }

  /**
   * Ranges over all the phi nodes in an edge.
   *
   * An edge e represents all non-phi instructions from e.first and all phi
   * instructions from e.second. This returns the latter.
   */
  llvm::iterator_range<llvm::BasicBlock::iterator>
  edgePhis(const edge &e) const {
    using namespace llvm;
    assert(edges().find(e) != edges().end());

    if (e.second == Fragment::EXIT) {
      // no phis but we need to return something
      BasicBlock::iterator end = e.first->begin();
      return iterator_range<BasicBlock::iterator>(end, end);
    } else {
      BasicBlock::iterator end = e.second->begin();

      // Find first non-phi. There is always one since a terminator
      // instruction is not a phi.
      while (isa<PHINode>(*end))
        ++end;

      return iterator_range<BasicBlock::iterator>(e.second->begin(), end);
    }
  }

  friend std::ostream &operator<<(std::ostream &out, const Fragment &frag);

  const std::set<llvm::BasicBlock *> &locations() const { return Locations_; }

  /**
   * Find all edges in this fragment that start in the given location.
   */
  std::vector<edge> edgesFrom(llvm::BasicBlock *location) const;

  /**
   * Find all edges in this fragment that end in the given location.
   */
  std::vector<edge> edgesTo(llvm::BasicBlock *location) const;

  /**
   * Checks wether the given Fragment is a predecessor of this fragment.
   * We define a predecessor to be a Fragment where one of its ending nodes
   * is contained in this Fragments start nodes
   */
  bool isPredecessor(const Fragment &frag) const;

  bool defines(llvm::Value *value) const;

  /**
   * True if this fragment includes the non-phi instructions in the ending
   * block. Otherwise, it's considered to end after all the phi instructions
   * but before any non-phis.
   */
  bool includesEndBody() const { return IncludesEndBody_; }

  /**
   * Check whether an b is reachable from a while following only instructions
   * in this fragment.
   *
   * The instruction a must be defined in this fragment. Note that if b is
   * not a phi node but b->getParent() == this->getEnd() then this function
   * may return true, even though defines(b) returns false.
   */
  bool reachable(llvm::Instruction *a, llvm::Instruction *b) const;

  const FunctionContext &getFunctionContext() const { return FunctionContext_; }
};
} // namespace symabs_ai

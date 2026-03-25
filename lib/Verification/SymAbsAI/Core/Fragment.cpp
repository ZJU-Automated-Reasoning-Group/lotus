/**
 * @file Fragment.cpp
 * @brief Implementation of Fragment, representing an acyclic control-flow
 * subgraph.
 *
 * A Fragment represents a subgraph of the control-flow graph between two
 * abstraction points (basic blocks). Fragments are used to decompose the CFG
 * for analysis, allowing the analyzer to compute transformers for manageable
 * pieces. This file implements queries about fragment structure: loop
 * detection, reachability, value definitions, and edge traversal.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Core/Fragment.h"

#include "Verification/SymAbsAI/Core/repr.h"

#include <queue>

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

namespace symabs_ai {
using namespace llvm;

llvm::BasicBlock *Fragment::EXIT = nullptr;

namespace {
/**
 * @brief Recursive DFS helper to detect loops in a fragment.
 *
 * Performs depth-first search through the fragment's edges to detect cycles.
 * The EXIT node can be ignored as it cannot be contained in a loop.
 *
 * @param edges Set of edges defining the fragment
 * @param[in,out] seen Set of visited basic blocks (cleared if loop found)
 * @param bb Current basic block being explored
 * @return true if a loop is detected, false otherwise
 */
bool findLoop(std::set<Fragment::edge> &edges,
              std::unordered_set<llvm::BasicBlock *> &seen,
              llvm::BasicBlock *bb) {
  if (seen.find(bb) != seen.end()) { // we found a bb that we have seen before
    seen.clear();
    return true;
  }

  seen.insert(bb);

  for (auto itr_bb_to = succ_begin(bb), end = succ_end(bb); itr_bb_to != end;
       ++itr_bb_to) {
    if (edges.find(std::make_pair(bb, *itr_bb_to)) != edges.end()) {
      // allowed edge
      if (findLoop(edges, seen, *itr_bb_to)) {
        return true;
      }
    }
  }

  seen.erase(bb);

  return false;
}
} // namespace

/**
 * @brief Check whether the fragment contains any loops.
 *
 * Determines if there exists a path from the start node to a loop using only
 * the edges in this fragment. Fragments should typically be acyclic for proper
 * analysis, but self-loops (Start == End) are allowed.
 *
 * @return true if the fragment contains a loop, false otherwise
 */
bool Fragment::hasLoops() {
  std::unordered_set<llvm::BasicBlock *> seen;
  return findLoop(Edges_, seen, Start_);
}

/**
 * @brief Output stream operator for Fragment.
 *
 * Prints a human-readable representation of the fragment showing the start
 * and end basic blocks. Format: "start-->end" or "start-->EXIT". If the
 * fragment includes the end body, appends a "+" suffix.
 *
 * @param out Output stream to write to
 * @param frag The fragment to print
 * @return Reference to the output stream
 */
std::ostream &operator<<(std::ostream &out, const Fragment &frag) {
  assert(frag.getStart() != nullptr);
  out << repr(frag.getStart()) << "-->";

  if (frag.getEnd() == Fragment::EXIT)
    out << "EXIT";
  else
    out << repr(frag.getEnd());

  if (frag.includesEndBody())
    out << "+";

  return out;
}

/**
 * @brief Get all outgoing edges from a location within the fragment.
 *
 * Returns all edges in this fragment that originate from the given basic block.
 * Includes the virtual edge to EXIT if the location has no successors and
 * the fragment includes that edge.
 *
 * @param location Basic block to get outgoing edges from (must be in fragment)
 * @return Vector of edges (source, destination) pairs
 */
std::vector<Fragment::edge> Fragment::edgesFrom(BasicBlock *location) const {
  assert(locations().find(location) != locations().end());
  std::vector<edge> result;

  if (location == Fragment::EXIT)
    return result;

  auto itr = succ_begin(location), end = succ_end(location);

  if (itr == end) {
    if (edges().find({location, EXIT}) != edges().end()) {
      result.push_back({location, EXIT});
    }
  }

  for (; itr != end; ++itr) {
    if (edges().find({location, *itr}) != edges().end()) {
      result.push_back({location, *itr});
    }
  }

  return result;
}

/**
 * @brief Get all incoming edges to a location within the fragment.
 *
 * Returns all edges in this fragment that terminate at the given basic block.
 *
 * @param location Basic block to get incoming edges to (must be in fragment)
 * @return Vector of edges (source, destination) pairs
 */
std::vector<Fragment::edge> Fragment::edgesTo(BasicBlock *location) const {
  assert(locations().find(location) != locations().end());
  std::vector<edge> result;

  for (edge e : edges()) {
    if (e.second == location)
      result.push_back(e);
  }

  return result;
}

/**
 * @brief Check if this fragment is a predecessor of another fragment.
 *
 * A fragment is a predecessor if its start block equals the other fragment's
 * end block, meaning control flow can transition directly from this fragment
 * to the other.
 *
 * @param frag The fragment to check against
 * @return true if this fragment's start equals frag's end, false otherwise
 */
bool Fragment::isPredecessor(const Fragment &frag) const {
  return getStart() == frag.getEnd();
}

/**
 * @brief Check if the fragment defines a given LLVM value.
 *
 * A value is defined by a fragment if:
 * - For PHI nodes: at least one incoming edge (pred->bb) is in the fragment
 * - For regular instructions: the instruction's basic block has an outgoing
 * edge in the fragment, OR the fragment includes the end body and the
 * instruction is in the end block
 *
 * @param value The LLVM value to check (must be an Instruction)
 * @return true if the fragment defines the value, false otherwise
 */
bool Fragment::defines(llvm::Value *value) const {
  auto *inst = dyn_cast<llvm::Instruction>(value);

  if (inst == nullptr)
    return false;

  llvm::BasicBlock *bb = inst->getParent();

  if (llvm::isa<llvm::PHINode>(inst)) {
    // A phi is defined on the edge prev_block->bb. Check whether at
    // least one such edge is in this fragment.
    for (auto itr = llvm::pred_begin(bb), end = llvm::pred_end(bb); itr != end;
         ++itr) {

      if (Edges_.find({*itr, bb}) != Edges_.end())
        return true;
    }
    return false;
  } else {
    // If this fragment includes the whole ending BB, it defines everything
    // in it.
    if (includesEndBody() && inst->getParent() == getEnd())
      return true;

    // Non-phi instructions are defined on the edge from bb.
    auto itr = llvm::succ_begin(bb), end = llvm::succ_end(bb);

    if (itr == end) {
      // check for a "virtual" edge bb->EXIT
      if (Edges_.find({bb, Fragment::EXIT}) != Edges_.end())
        return true;
    } else {
      for (; itr != end; ++itr) {
        if (Edges_.find({bb, *itr}) != Edges_.end())
          return true;
      }
    }
    return false;
  }
}

/**
 * @brief Check if instruction b is reachable from instruction a within the
 * fragment.
 *
 * Performs a BFS traversal from a's basic block to determine if b's basic block
 * is reachable using only edges in the fragment. Handles special cases for:
 * - Instructions in the same basic block (uses instruction order)
 * - Self-looping fragments (Start == End) with PHI ordering
 * - PHI nodes at the start block
 *
 * @param a Source instruction (must be defined by the fragment)
 * @param b Target instruction
 * @return true if b is reachable from a within the fragment, false otherwise
 */
bool Fragment::reachable(Instruction *a, Instruction *b) const {
  assert(defines(a));

  // handle as a special case if a and b are in the same block
  if (a->getParent() == b->getParent()) {
    std::vector<Instruction *> insts;

    if (getStart() == getEnd() && a->getParent() == getStart()) {
      // This block is both a starting and ending block of this fragment.
      // In this case, the phi instructions actually come *after* the
      // non phis.
      for (auto &x : *getStart()) {
        if (!isa<PHINode>(x))
          insts.push_back(&x);
      }

      for (auto &x : *getStart()) {
        if (isa<PHINode>(x))
          insts.push_back(&x);
      }
    } else {
      for (auto &x : a->getParent()->getInstList())
        insts.push_back(&x);
    }

    auto a_itr = std::find(insts.begin(), insts.end(), a);
    auto b_itr = std::find(insts.begin(), insts.end(), b);
    return a_itr <= b_itr;
  }

  if (!defines(b))
    return false;

  if (Start_ == b->getParent() && !isa<PHINode>(b)) {
    // If b is in Start_ and it is no PHINode, it can only be reachable
    // from a if a is also in Start_, which would have been handled before.
    return false;
  }
  // Perform a BFS from a. Since fragments are acyclic, it's not necessary to
  // keep track of visited locations.
  std::queue<BasicBlock *> queue;
  queue.push(a->getParent());

  bool first_time_start = true;
  while (!queue.empty()) {
    BasicBlock *bb = queue.front();
    queue.pop();

    if (bb == b->getParent()) {
      // If b is a phi, it's reachable as long as bb is reachable. If b is
      // not a phi, there must be at least one outgoing edge from bb since
      // this->defines(b).
      return true;
    }

    bool edge_from_end_necessary = (Start_ == End_) &&
                                   (a->getParent() == Start_) &&
                                   !isa<PHINode>(a) && first_time_start;
    // Since Fragments have to be (almost) acyclic, the only possibility of
    // a transition from End_ in the Fragment is that End_ == Start_.
    // Adding the destination of such a transition to the queue here is
    // only desirable if
    //  - the start instruction a of the BFS lies in Start_
    //  - it is not a PHINode
    // (this means that the non-phi part of the block is actually needed).
    // It is necessary to assure that such successors are added exactly
    // once as otherwise, with Start_ == End_, the BFS might diverge.
    if (End_ != bb || edge_from_end_necessary) {
      // Note that we don't add Fragment::EXIT to the queue since it's not
      // possible for b->getParent() to be equal to Fragment::EXIT
      for (auto itr = succ_begin(bb), end = succ_end(bb); itr != end; ++itr) {
        if (Edges_.find({bb, *itr}) != Edges_.end())
          queue.push(*itr);
      }
      // if the above mentioned case (with Start_ == End_ and a really in
      // Start_) occurs, the successors of Start_ are added in the first
      // iteration, therefore setting the flag to false here is correct.
      first_time_start = false;
    }
  }

  return false;
}
} // namespace symabs_ai

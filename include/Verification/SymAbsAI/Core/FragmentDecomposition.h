/**
 * @file FragmentDecomposition.h
 * @brief Utilities for partitioning a function's CFG into acyclic fragments
 *        used as units of abstract interpretation.
 */
#pragma once

#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"

#include <iostream>
#include <set>
#include <vector>

namespace symabs_ai {
/**
 * Computes and stores a decomposition of a function into `Fragment`s.
 *
 * Different strategies choose abstraction points (e.g. every edge, loop
 * headers/bodies, whole-function) and then build maximal acyclic fragments
 * between them to balance precision and solver cost.
 */
class FragmentDecomposition {
public:
  enum strategy { Edges, Function, Headers, Body, Backedges };

private:
  std::vector<Fragment> Fragments_;
  const FunctionContext &FunctionContext_;

  FragmentDecomposition(const FunctionContext &fctx) : FunctionContext_(fctx) {}

public:
  // noncopyable but moveable
  FragmentDecomposition(const FragmentDecomposition &) = delete;
  FragmentDecomposition &operator=(const FragmentDecomposition &) = delete;
  FragmentDecomposition(FragmentDecomposition &&) = default;
  FragmentDecomposition &operator=(FragmentDecomposition &&) = delete;

  static std::set<llvm::BasicBlock *> GetAbstractionPoints(llvm::Function *,
                                                           strategy);

  /**
   * Create a fragment between two locations inside another fragment.
   *
   * Distinct locations `start` and `end` need to belong to the fragment
   * `parent` and there must be a path between them in `parent`. The result
   * is a new fragment that uses a subset of edges of `parent` connecting
   * `start` and `end`.
   *
   * The optional parameter `includes_end_body` can be used to create a sub
   * fragment that includes non-phi instructions of the basic block `end`.
   * If false, the sub-fragment is considered to end in the middle of this
   * block, i.e., after all phi instructions but before any non-phis.
   */
  static Fragment SubFragment(const Fragment &parent, llvm::BasicBlock *start,
                              llvm::BasicBlock *end,
                              bool includes_end_body = false);

  /**
   * Create a fragment comprised of non-phi instructions of a given basic
   * block.
   *
   * The resulting fragment will start and end in the given location,
   * including only the non-phi instructions. It shall contain no edges.
   */
  static Fragment FragmentForBody(const FunctionContext &fctx,
                                  llvm::BasicBlock *location);

  static FragmentDecomposition
  ForAbstractionPoints(const FunctionContext &fctx,
                       const std::set<llvm::BasicBlock *> &abstraction_points);

  static FragmentDecomposition For(const FunctionContext &, strategy);
  static FragmentDecomposition For(const FunctionContext &);

  using iterator = std::vector<Fragment>::const_iterator;
  iterator begin() const { return Fragments_.begin(); }
  iterator end() const { return Fragments_.end(); }

  std::set<llvm::BasicBlock *> abstractionPoints();

  friend std::ostream &operator<<(std::ostream &out,
                                  const FragmentDecomposition &fdec);
};
} // namespace symabs_ai

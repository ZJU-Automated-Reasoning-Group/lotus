#ifndef CHECKER_PULSE_PULSEDISJUNCTIVEDOMAIN_H
#define CHECKER_PULSE_PULSEDISJUNCTIVEDOMAIN_H

#include "Checker/Pulse/Domain/PulseDomain.h"

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/BasicBlock.h>

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace pulse {

/**
 * DisjunctiveDomain: manages multiple disjunctive states
 * Implements proper disjunctive abstract domain with widening
 */
class DisjunctiveDomain {
public:
  // A disjunct is a pair of (execution state, path context)
  // For now, path context is just the basic block
  struct Disjunct {
    ExecutionDomain state;
    const llvm::BasicBlock *path_context;

    Disjunct() : path_context(nullptr) {}

    Disjunct(ExecutionDomain s, const llvm::BasicBlock *ctx)
        : state(std::move(s)), path_context(ctx) {}
  };

private:
  // Disjuncts are tracked per basic block. This matters: joining/widening
  // should only combine states that are at the same program point (block
  // entry).
  std::map<const llvm::BasicBlock *, std::vector<Disjunct>> disjuncts_by_block_;
  static constexpr size_t kMaxDisjuncts = 10;
  static constexpr unsigned kWidenThreshold = 3; // Widen after 3 iterations
  static constexpr size_t kWidenKeepDisjuncts = 4;

  // Track iterations per block for widening
  std::map<const llvm::BasicBlock *, unsigned> block_iterations_;

public:
  DisjunctiveDomain() = default;

  /**
   * Add a disjunct
   */
  void add(const llvm::BasicBlock *at_block, ExecutionDomain state,
           const llvm::BasicBlock *path_context);

  /**
   * Get disjuncts recorded at a given block
   */
  const std::vector<Disjunct> &
  getDisjuncts(const llvm::BasicBlock *at_block) const;
  std::vector<Disjunct> &getDisjuncts(const llvm::BasicBlock *at_block);

  /**
   * Get total number of disjuncts across all blocks
   */
  size_t size() const;

  /**
   * Join disjuncts at a block entry
   * Returns the joined state (or first state if only one)
   */
  ExecutionDomain joinAtBlock(const llvm::BasicBlock *BB);

  /**
   * Widen: apply widening operator if needed
   */
  void widen(const llvm::BasicBlock *BB);

  /**
   * Check if we should widen at this block
   */
  bool shouldWiden(const llvm::BasicBlock *BB) const;

  /**
   * Clear disjuncts
   */
  void clear() {
    disjuncts_by_block_.clear();
    block_iterations_.clear();
  }

  /**
   * Check if empty
   */
  bool empty() const { return disjuncts_by_block_.empty(); }

  /**
   * Limit disjuncts to max
   */
  void limitDisjuncts(const llvm::BasicBlock *at_block);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEDISJUNCTIVEDOMAIN_H

#ifndef CHECKER_PULSE_PULSELOOPABSTRACTION_H
#define CHECKER_PULSE_PULSELOOPABSTRACTION_H

#include "Checker/Pulse/Domain/PulseDomain.h"

#include <map>
#include <set>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>

namespace llvm {
class Loop;
class LoopInfo;
class BasicBlock;
} // namespace llvm

namespace pulse {

/**
 * LoopAbstraction: handles loop abstraction for Pulse analysis
 * Tracks loop headers, implements widening at loop headers, and infers
 * invariants Production-ready implementation aligned with Infer's
 * PulseLoopHeaderInfo
 */
class LoopAbstraction {
private:
  /**
   * IterationInfo: tracks information about a single loop iteration
   */
  struct IterationInfo {
    unsigned timestamp;      // Timestamp/sequence number of this iteration
    PulseFormula path_stamp; // Path condition stamp for this iteration

    IterationInfo() : timestamp(0) {}
    IterationInfo(unsigned ts, const PulseFormula &ps)
        : timestamp(ts), path_stamp(ps.clone()) {}
  };

  /**
   * LoopInfo: comprehensive information about a loop header
   */
  struct LoopInfo {
    const llvm::Loop *loop;
    unsigned iterations;          // Number of times we've visited this header
    ExecutionDomain header_state; // State at loop header (first iteration)
    ExecutionDomain entry_state;  // State when entering loop
    PulseFormula local_path_condition;          // Path condition local to loop
    std::vector<IterationInfo> iteration_stack; // Stack of iteration info

    LoopInfo() : loop(nullptr), iterations(0) {}
  };

  std::map<const llvm::BasicBlock *, LoopInfo> loop_headers_;
  unsigned current_timestamp_; // Global timestamp counter

  static constexpr unsigned kWidenThreshold = 2; // Widen after 2 iterations
  static constexpr unsigned kMaxWidenIterations = 10;
  static constexpr unsigned kMaxInvariantIterations =
      5; // Max iterations for invariant inference

public:
  LoopAbstraction() : current_timestamp_(1) {}

  /**
   * Initialize loop information from LLVM LoopInfo
   */
  void initialize(const llvm::LoopInfo &LI);

  /**
   * Check if a basic block is a loop header
   */
  bool isLoopHeader(const llvm::BasicBlock *BB) const;

  /**
   * Get loop for a header
   */
  const llvm::Loop *getLoop(const llvm::BasicBlock *BB) const;

  /**
   * Check if we should widen at this header
   */
  bool shouldWiden(const llvm::BasicBlock *BB) const;

  /**
   * Record visit to loop header
   * Returns true if we should widen
   */
  bool visitHeader(const llvm::BasicBlock *BB, const ExecutionDomain &state);

  /**
   * Widen: apply widening operator at loop header
   */
  ExecutionDomain widen(const llvm::BasicBlock *BB,
                        const ExecutionDomain &current_state);

  /**
   * Check if basic block is in a loop
   */
  bool isInLoop(const llvm::BasicBlock *BB) const;

  /**
   * Get all loop headers
   */
  std::set<const llvm::BasicBlock *> getLoopHeaders() const;

  /**
   * Infer loop invariant: attempt to infer an invariant for a loop
   * Returns the inferred invariant state, or empty if inference failed
   */
  llvm::Optional<ExecutionDomain>
  inferInvariant(const llvm::BasicBlock *BB, const ExecutionDomain &entry_state,
                 const ExecutionDomain &current_state);

  /**
   * Check if loop invariant inference is in progress for a header
   */
  bool isInferringInvariant(const llvm::BasicBlock *BB) const;

  /**
   * Push loop iteration info: record a new iteration with path stamp
   */
  void pushLoopInfo(const llvm::BasicBlock *BB, unsigned timestamp,
                    const PulseFormula &path_condition);

  /**
   * Initialize loop info for a header
   */
  void initLoopInfo(const llvm::BasicBlock *BB);

  /**
   * Remove loop info (when exiting loop)
   */
  void removeLoopInfo(const llvm::BasicBlock *BB);

  /**
   * Get iteration index for a loop header
   */
  unsigned getIterationIndex(const llvm::BasicBlock *BB) const;

  /**
   * Check if previous iteration had same path stamp (for convergence detection)
   */
  bool hasPreviousIterationSamePathStamp(const llvm::BasicBlock *BB) const;

  /**
   * Check if current iteration has empty path stamp (for widening)
   */
  bool isCurrentIterationEmptyPathStamp(const llvm::BasicBlock *BB) const;

  /**
   * Map formulas in loop info (for path condition updates)
   */
  void mapFormulas(const llvm::BasicBlock *BB,
                   std::function<PulseFormula(const PulseFormula &)> f);

  /**
   * Get current timestamp and increment
   */
  unsigned getNextTimestamp() { return current_timestamp_++; }

  /**
   * Reset timestamp counter
   */
  void resetTimestamp() { current_timestamp_ = 1; }

  /**
   * Get entry state for a loop header
   */
  const ExecutionDomain &getEntryState(const llvm::BasicBlock *BB) const;
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSELOOPABSTRACTION_H

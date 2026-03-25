#ifndef CHECKER_PULSE_PULSEPATHCONTEXT_H
#define CHECKER_PULSE_PULSEPATHCONTEXT_H

#include <cstdint>

#include <llvm/IR/BasicBlock.h>

namespace pulse {

/**
 * PathContext: tracks path-sensitive information during analysis.
 * Matches Infer's PulsePathContext design.
 */
class PathContext {
private:
  uint64_t timestamp_; // Step number in intra-procedural analysis
  bool is_non_disj_; // Whether we're in non-disjunctive (over-approximate) part

public:
  PathContext() : timestamp_(0), is_non_disj_(false) {}

  PathContext(uint64_t timestamp, bool is_non_disj)
      : timestamp_(timestamp), is_non_disj_(is_non_disj) {}

  uint64_t getTimestamp() const { return timestamp_; }
  bool isNonDisj() const { return is_non_disj_; }

  void setTimestamp(uint64_t ts) { timestamp_ = ts; }
  void setNonDisj(bool val) { is_non_disj_ = val; }

  /**
   * Post-execution: increment timestamp after instruction
   */
  PathContext postExecInstr() const {
    PathContext result = *this;
    result.timestamp_++;
    return result;
  }

  /**
   * Join two path contexts.
   */
  static PathContext join(const PathContext &lhs, const PathContext &rhs) {
    // Take maximum timestamp and OR the non-disj flags
    return PathContext(std::max(lhs.timestamp_, rhs.timestamp_),
                       lhs.is_non_disj_ || rhs.is_non_disj_);
  }

  /**
   * Initial path context.
   */
  static PathContext initial() { return PathContext(0, false); }

  bool operator==(const PathContext &other) const {
    return timestamp_ == other.timestamp_ && is_non_disj_ == other.is_non_disj_;
  }

  bool operator<(const PathContext &other) const {
    if (timestamp_ != other.timestamp_)
      return timestamp_ < other.timestamp_;
    return is_non_disj_ < other.is_non_disj_;
  }
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEPATHCONTEXT_H

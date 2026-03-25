//===-- Verification/Sifa/Statistics/SifaStats.h --------------------------===//
//
// Statistics for Sifa (Ultimate-aligned).
//
// Ultimate's SifaStats uses counters, stopwatches, and max-timers. We support
// nested start/stop for stopwatches and add/startMax/stopMax for alignment.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_STATISTICS_SIFASTATS_H
#define LOTUS_VERIFICATION_SIFA_STATISTICS_SIFASTATS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace sifa {

class SifaStats {
public:
  /// Ultimate-aligned statistics keys (KeyType: COUNTER, TIMER, MAX_TIMER).
  enum class Key : std::uint16_t {
    OVERALL_TIME, // TIMER

    ICFG_INTERPRETER_ENTERED_PROCEDURES,           // COUNTER
    DAG_INTERPRETER_EARLY_EXIT_QUERIES_NONTRIVIAL, // COUNTER
    DAG_INTERPRETER_EARLY_EXITS,                   // COUNTER

    TOOLS_POST_APPLICATIONS,           // COUNTER
    TOOLS_POST_TIME,                   // TIMER
    TOOLS_POST_CALL_APPLICATIONS,      // COUNTER
    TOOLS_POST_CALL_TIME,              // TIMER
    TOOLS_POST_RETURN_APPLICATIONS,    // COUNTER
    TOOLS_POST_RETURN_TIME,            // TIMER
    TOOLS_QUANTIFIERELIM_APPLICATIONS, // COUNTER
    TOOLS_QUANTIFIERELIM_TIME,         // TIMER
    TOOLS_QUANTIFIERELIM_MAX_TIME,     // MAX_TIMER

    FLUID_QUERY_TIME,  // TIMER
    FLUID_QUERIES,     // COUNTER
    FLUID_YES_ANSWERS, // COUNTER

    DOMAIN_JOIN_APPLICATIONS,       // COUNTER
    DOMAIN_JOIN_TIME,               // TIMER
    DOMAIN_ALPHA_APPLICATIONS,      // COUNTER
    DOMAIN_ALPHA_TIME,              // TIMER
    DOMAIN_WIDEN_APPLICATIONS,      // COUNTER
    DOMAIN_WIDEN_TIME,              // TIMER
    DOMAIN_ISSUBSETEQ_APPLICATIONS, // COUNTER
    DOMAIN_ISSUBSETEQ_TIME,         // TIMER
    DOMAIN_ISBOTTOM_APPLICATIONS,   // COUNTER
    DOMAIN_ISBOTTOM_TIME,           // TIMER

    LOOP_SUMMARIZER_APPLICATIONS,         // COUNTER
    LOOP_SUMMARIZER_CACHE_MISSES,         // COUNTER
    LOOP_SUMMARIZER_OVERALL_TIME,         // TIMER
    LOOP_SUMMARIZER_NEW_COMPUTATION_TIME, // TIMER
    LOOP_SUMMARIZER_FIXPOINT_ITERATIONS,  // COUNTER

    CALL_SUMMARIZER_APPLICATIONS,         // COUNTER
    CALL_SUMMARIZER_CACHE_MISSES,         // COUNTER
    CALL_SUMMARIZER_OVERALL_TIME,         // TIMER
    CALL_SUMMARIZER_NEW_COMPUTATION_TIME, // TIMER

    PROCEDURE_GRAPH_BUILDER_TIME, // TIMER

    PATH_EXPR_TIME,                  // TIMER
    REGEX_TO_DAG_TIME,               // TIMER
    DAG_COMPRESSION_TIME,            // TIMER
    DAG_COMPRESSION_PROCESSED_NODES, // COUNTER
    DAG_COMPRESSION_RETAINED_NODES,  // COUNTER

    // Legacy / convenience alias (same as FLUID_YES_ANSWERS usage in wrappers)
    FLUID_ABSTRACTIONS_APPLIED, // COUNTER
  };

  void increment(Key k, std::uint64_t by = 1);
  void add(Key k, std::uint64_t summand);

  /// Start (or nest) a stopwatch for \p k.
  ///
  /// Stopwatches are *nestable*: multiple start() calls must be paired with
  /// stop() calls; only the outermost interval contributes to the duration.
  /// Calls to start()/stop() for non-stopwatch keys are allowed but have no
  /// special meaning beyond accumulating in the internal maps.
  void start(Key k);
  /// Stop (or un-nest) a stopwatch for \p k. If \p k was not running, no-op.
  void stop(Key k);
  /// Start a MAX_TIMER interval for \p k (Ultimate-aligned).
  ///
  /// Semantics are the same as start(), but stopMax() additionally updates the
  /// maximum single-interval duration observed so far for \p k.
  void startMax(Key k);
  /// Stop a MAX_TIMER interval for \p k (Ultimate-aligned). If \p k was not
  /// running, no-op.
  void stopMax(Key k);

  std::uint64_t counter(Key k) const;
  std::chrono::nanoseconds duration(Key k) const;
  std::chrono::nanoseconds maxDuration(Key k) const;

  /// Ultimate-aligned: getValue(keyName). Returns counter value or duration
  /// (ns) as uint64_t.
  std::uint64_t getValue(const std::string &keyName) const;
  /// Ultimate-aligned: getValue(Key). Same as counter(k) for COUNTER, duration
  /// count for TIMER.
  std::uint64_t getValue(Key k) const;
  /// Ultimate-aligned: getKeys() — all key names.
  std::vector<std::string> getKeys() const;
  /// Ultimate-aligned: getStopwatches() — key names that are stopwatches
  /// (TIMER/MAX_TIMER).
  std::vector<std::string> getStopwatches() const;

private:
  static bool keyIsStopwatch(Key k);
  using Clock = std::chrono::steady_clock;

  struct KeyHash {
    std::size_t operator()(Key k) const;
  };

  std::unordered_map<Key, std::uint64_t, KeyHash> counters_;
  std::unordered_map<Key, Clock::time_point, KeyHash> starts_;
  std::unordered_map<Key, std::chrono::nanoseconds, KeyHash> durations_;
  std::unordered_map<Key, int, KeyHash> stopwatchNesting_;
  // Max-timer: track current interval start and max single interval so far.
  std::unordered_map<Key, Clock::time_point, KeyHash> maxStarts_;
  std::unordered_map<Key, std::chrono::nanoseconds, KeyHash> maxDurations_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_STATISTICS_SIFASTATS_H

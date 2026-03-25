/// Pointer analysis metrics for comparing precision and soundness across
/// analyses
#pragma once

#include <cstddef>
#include <cstdint>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {

class AliasAnalysisWrapper;

/**
 * Metrics over pointer analysis results, inspired by Java points-to metrics
 * (e.g. hybrid context-sensitivity, PLDI'13) but adapted for C/C++.
 *
 * Used to compare analyses: lower counts (e.g. avg pts size, poly calls)
 * generally mean higher precision. All analyses are intended sound
 * (over-approximate); these metrics measure how much over-approximation.
 *
 * See lib/Alias/METRICS.md for design and Java vs C/C++ mapping.
 */
struct PointerAnalysisMetrics {
  // --- Points-to set size (when backend exposes pts) ---
  uint64_t num_pointers_tracked = 0; ///< Pointers for which we had pts/size
  uint64_t total_points_to_size = 0; ///< Sum of pts set sizes
  uint64_t max_points_to_size = 0;   ///< Max pts set size
  double avg_points_to_size =
      0.0; ///< total_points_to_size / num_pointers_tracked
  double median_points_to_size = 0.0; ///< Median (if computed)

  // --- Call graph / indirect calls ---
  uint64_t num_direct_call_edges = 0; ///< (call site, callee) for direct calls
  uint64_t num_indirect_call_sites = 0; ///< Call sites with indirect callee
  uint64_t num_indirect_call_edges =
      0; ///< Resolved (call site, callee) for indirect
  uint64_t num_poly_indirect_calls = 0; ///< Indirect sites with >1 target
  double avg_targets_per_indirect =
      0.0; ///< num_indirect_call_edges / num_indirect_call_sites

  // --- Alias query metrics (use-site pairs, possibly capped) ---
  uint64_t num_alias_pairs_queried =
      0;                     ///< Pairs actually queried (may be capped)
  uint64_t num_no_alias = 0; ///< Pairs proved non-aliasing
  uint64_t num_must_alias = 0;
  uint64_t num_may_alias = 0;
  uint64_t num_partial_alias = 0;

  /// Total call graph edges (direct + indirect) from this analysis
  uint64_t totalCallGraphEdges() const {
    return num_direct_call_edges + num_indirect_call_edges;
  }

  /// Fraction of queried pairs that are NoAlias (0 if none queried)
  double fractionNoAlias() const {
    if (num_alias_pairs_queried == 0)
      return 0.0;
    return static_cast<double>(num_no_alias) /
           static_cast<double>(num_alias_pairs_queried);
  }
};

/** Options for metrics collection. Alias-pair metrics can be expensive. */
struct CollectMetricsOptions {
  /// Max alias pairs to query (use-site pairs). 0 = skip alias-pair metrics.
  /// Default 50k keeps cost reasonable; increase for more precision in the
  /// sample.
  uint64_t max_alias_pairs = 50000;
};

/**
 * Collect metrics from an alias analysis wrapper over the given module.
 *
 * Uses the wrapper's getPointsToSet / getPointsToSetSize (when available)
 * and iterates pointers and indirect call sites in M. Optionally collects
 * alias-pair metrics over use-site pairs (pointer operands at each
 * instruction), capped by options.max_alias_pairs to avoid O(n^2) cost.
 *
 * @param aa      Initialized AliasAnalysisWrapper (analysis already run)
 * @param M       Module to iterate (pointers and call sites)
 * @param out     Filled with collected metrics
 * @param options Max alias pairs to query (0 = skip); default 50k
 */
void collectMetricsFromWrapper(AliasAnalysisWrapper &aa, llvm::Module &M,
                               PointerAnalysisMetrics &out,
                               CollectMetricsOptions options = {});

} // namespace lotus

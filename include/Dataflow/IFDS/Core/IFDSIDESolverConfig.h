/*
 * IFDS/IDE Solver Configuration
 *
 * Configuration options for the IFDS/IDE solving process (aligned with Phasar's
 * IFDSIDESolverConfig where applicable).
 */

#pragma once

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <cstdint>

namespace ifds {

enum class SolverConfigOptions : uint32_t {
  None = 0,
  FollowReturnsPastSeeds = 1,
  RecordEdges = 2,
  ComputeValues = 4,    // Compute IDE values (can disable for IFDS-only)
  EnableStatistics = 8, // Collect detailed statistics
  EnableProgressReporting = 16,   // Show progress during analysis
  EnableFlowFunctionCaching = 32, // Cache flow function results
  EnableEdgeFunctionCaching = 64, // Cache edge function compositions
  All = ~0U
};

/// Configuration for IFDS/IDE solver behavior.
struct IFDSIDESolverConfig {
  IFDSIDESolverConfig() = default;
  explicit IFDSIDESolverConfig(SolverConfigOptions options) noexcept
      : m_options(static_cast<uint32_t>(options)) {}

  /// When true, propagate return flow to callers' return sites even when the
  /// callee (start_fact) had no incoming call edge (e.g. entry-point function
  /// returning). Default: false.
  bool follow_returns_past_seeds() const {
    return (m_options & static_cast<uint32_t>(
                            SolverConfigOptions::FollowReturnsPastSeeds)) != 0;
  }
  void set_follow_returns_past_seeds(bool set = true) {
    if (set)
      m_options |=
          static_cast<uint32_t>(SolverConfigOptions::FollowReturnsPastSeeds);
    else
      m_options &=
          ~static_cast<uint32_t>(SolverConfigOptions::FollowReturnsPastSeeds);
  }

  /// When true, record computed path edges for debugging/export. May increase
  /// memory. Default: false.
  bool record_edges() const {
    return (m_options &
            static_cast<uint32_t>(SolverConfigOptions::RecordEdges)) != 0;
  }
  void set_record_edges(bool set = true) {
    if (set)
      m_options |= static_cast<uint32_t>(SolverConfigOptions::RecordEdges);
    else
      m_options &= ~static_cast<uint32_t>(SolverConfigOptions::RecordEdges);
  }

  void set_config(SolverConfigOptions opt) {
    m_options = static_cast<uint32_t>(opt);
  }

  /// Compute IDE values (can disable for IFDS-only mode for performance).
  /// Default: true.
  bool compute_values() const {
    return (m_options &
            static_cast<uint32_t>(SolverConfigOptions::ComputeValues)) != 0;
  }
  void set_compute_values(bool set = true) {
    if (set)
      m_options |= static_cast<uint32_t>(SolverConfigOptions::ComputeValues);
    else
      m_options &= ~static_cast<uint32_t>(SolverConfigOptions::ComputeValues);
  }

  /// Enable detailed statistics collection. Default: false.
  bool enable_statistics() const {
    return (m_options &
            static_cast<uint32_t>(SolverConfigOptions::EnableStatistics)) != 0;
  }
  void set_enable_statistics(bool set = true) {
    if (set)
      m_options |= static_cast<uint32_t>(SolverConfigOptions::EnableStatistics);
    else
      m_options &=
          ~static_cast<uint32_t>(SolverConfigOptions::EnableStatistics);
  }

  /// Enable progress reporting during analysis. Default: false.
  bool enable_progress_reporting() const {
    return (m_options & static_cast<uint32_t>(
                            SolverConfigOptions::EnableProgressReporting)) != 0;
  }
  void set_enable_progress_reporting(bool set = true) {
    if (set)
      m_options |=
          static_cast<uint32_t>(SolverConfigOptions::EnableProgressReporting);
    else
      m_options &=
          ~static_cast<uint32_t>(SolverConfigOptions::EnableProgressReporting);
  }

  /// Enable flow function result caching. Default: true (enabled).
  bool enable_flow_function_caching() const {
    return (m_options & static_cast<uint32_t>(
                            SolverConfigOptions::EnableFlowFunctionCaching)) !=
           0;
  }
  void set_enable_flow_function_caching(bool set = true) {
    if (set)
      m_options |=
          static_cast<uint32_t>(SolverConfigOptions::EnableFlowFunctionCaching);
    else
      m_options &= ~static_cast<uint32_t>(
          SolverConfigOptions::EnableFlowFunctionCaching);
  }

  /// Enable edge function caching (composition/join). Default: true (enabled).
  bool enable_edge_function_caching() const {
    return (m_options & static_cast<uint32_t>(
                            SolverConfigOptions::EnableEdgeFunctionCaching)) !=
           0;
  }
  void set_enable_edge_function_caching(bool set = true) {
    if (set)
      m_options |=
          static_cast<uint32_t>(SolverConfigOptions::EnableEdgeFunctionCaching);
    else
      m_options &= ~static_cast<uint32_t>(
          SolverConfigOptions::EnableEdgeFunctionCaching);
  }

  /// Preset: Fast configuration (minimal tracking, aggressive caching).
  static IFDSIDESolverConfig fast_config() {
    IFDSIDESolverConfig config;
    config.m_options =
        static_cast<uint32_t>(SolverConfigOptions::ComputeValues) |
        static_cast<uint32_t>(SolverConfigOptions::EnableFlowFunctionCaching) |
        static_cast<uint32_t>(SolverConfigOptions::EnableEdgeFunctionCaching);
    return config;
  }

  /// Preset: Debug configuration (full tracking, statistics, progress).
  static IFDSIDESolverConfig debug_config() {
    IFDSIDESolverConfig config;
    config.m_options =
        static_cast<uint32_t>(SolverConfigOptions::ComputeValues) |
        static_cast<uint32_t>(SolverConfigOptions::RecordEdges) |
        static_cast<uint32_t>(SolverConfigOptions::EnableStatistics) |
        static_cast<uint32_t>(SolverConfigOptions::EnableProgressReporting) |
        static_cast<uint32_t>(SolverConfigOptions::EnableFlowFunctionCaching) |
        static_cast<uint32_t>(SolverConfigOptions::EnableEdgeFunctionCaching);
    return config;
  }

  /// Automatically construct and inject alias analysis when none was provided
  /// by the client analysis.
  /// Default: false (Phasar-style explicit alias wiring).
  bool auto_inject_alias_analysis() const {
    return m_auto_inject_alias_analysis;
  }
  void set_auto_inject_alias_analysis(bool enable = true) {
    m_auto_inject_alias_analysis = enable;
  }

  /// Alias backend used when auto injection is enabled.
  const lotus::AAConfig &alias_analysis_config() const {
    return m_alias_analysis_config;
  }
  void set_alias_analysis_config(const lotus::AAConfig &cfg) {
    m_alias_analysis_config = cfg;
  }

  static lotus::AAConfig default_alias_analysis_config() {
    return lotus::AAConfig::SparrowAA_NoCtx();
  }

private:
  // Default: ComputeValues + caching enabled
  uint32_t m_options =
      static_cast<uint32_t>(SolverConfigOptions::ComputeValues) |
      static_cast<uint32_t>(SolverConfigOptions::EnableFlowFunctionCaching) |
      static_cast<uint32_t>(SolverConfigOptions::EnableEdgeFunctionCaching);
  // Phasar-style default: alias-aware analyses explicitly receive alias info.
  bool m_auto_inject_alias_analysis = false;
  lotus::AAConfig m_alias_analysis_config = default_alias_analysis_config();
};

} // namespace ifds

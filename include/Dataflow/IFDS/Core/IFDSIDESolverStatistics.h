/*
 * IFDS/IDE Solver Statistics
 *
 * Comprehensive statistics collection for solver performance analysis,
 * debugging, and optimization.
 */

#pragma once

#include <chrono>
#include <unordered_map>

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// Solver Statistics
// ============================================================================

struct IFDSIDESolverStatistics {
  // ========== Timing Statistics ==========
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;
  double total_time_seconds = 0.0;
  double initialization_time_seconds = 0.0;
  double tabulation_time_seconds = 0.0;
  double value_computation_time_seconds = 0.0;

  // ========== Worklist Statistics ==========
  size_t path_edges_processed = 0;
  size_t path_edges_propagated = 0;
  size_t path_edges_total = 0; // Total unique path edges
  size_t max_worklist_size = 0;
  size_t worklist_additions = 0;

  // ========== Flow Function Statistics ==========
  size_t normal_flow_applications = 0;
  size_t call_flow_applications = 0;
  size_t return_flow_applications = 0;
  size_t call_to_return_flow_applications = 0;
  size_t summary_flow_applications = 0;

  // Flow function cache statistics
  size_t flow_function_cache_hits = 0;
  size_t flow_function_cache_misses = 0;

  // ========== Edge Function Statistics (IDE only) ==========
  size_t edge_function_applications = 0;
  size_t edge_function_compositions = 0;
  size_t edge_function_joins = 0;
  size_t identity_edge_functions = 0;

  // Edge function cache statistics
  size_t edge_function_cache_hits = 0;
  size_t edge_function_cache_misses = 0;
  size_t compose_cache_hits = 0;
  size_t compose_cache_misses = 0;
  size_t join_cache_hits = 0;
  size_t join_cache_misses = 0;

  // ========== Summary Edge Statistics ==========
  size_t summary_edges_created = 0;
  size_t summary_edges_reused = 0;
  size_t summary_edges_total = 0;

  // ========== Program Analysis Statistics ==========
  size_t functions_analyzed = 0;
  size_t instructions_analyzed = 0;
  size_t call_sites_analyzed = 0;
  size_t indirect_call_sites = 0;

  // ========== Result Statistics ==========
  size_t facts_at_entry_total = 0;
  size_t facts_at_exit_total = 0;
  size_t values_computed = 0; // IDE only
  size_t max_facts_at_instruction = 0;

  // Per-function statistics
  std::unordered_map<const llvm::Function *, size_t> facts_per_function;
  std::unordered_map<const llvm::Function *, double> time_per_function;

  // ========== Memory Statistics ==========
  size_t jump_functions_stored = 0; // IDE only
  size_t estimated_memory_bytes = 0;

  // ========== Derived Metrics ==========

  double flow_function_cache_hit_rate() const {
    size_t total = flow_function_cache_hits + flow_function_cache_misses;
    return total > 0 ? static_cast<double>(flow_function_cache_hits) /
                           static_cast<double>(total)
                     : 0.0;
  }

  double edge_function_cache_hit_rate() const {
    size_t total = edge_function_cache_hits + edge_function_cache_misses;
    return total > 0 ? static_cast<double>(edge_function_cache_hits) /
                           static_cast<double>(total)
                     : 0.0;
  }

  double compose_cache_hit_rate() const {
    size_t total = compose_cache_hits + compose_cache_misses;
    return total > 0 ? static_cast<double>(compose_cache_hits) /
                           static_cast<double>(total)
                     : 0.0;
  }

  double join_cache_hit_rate() const {
    size_t total = join_cache_hits + join_cache_misses;
    return total > 0 ? static_cast<double>(join_cache_hits) /
                           static_cast<double>(total)
                     : 0.0;
  }

  double summary_reuse_rate() const {
    size_t total = summary_edges_created + summary_edges_reused;
    return total > 0 ? static_cast<double>(summary_edges_reused) /
                           static_cast<double>(total)
                     : 0.0;
  }

  double avg_facts_per_instruction() const {
    return instructions_analyzed > 0
               ? static_cast<double>(facts_at_exit_total) /
                     static_cast<double>(instructions_analyzed)
               : 0.0;
  }

  double path_edges_per_second() const {
    return total_time_seconds > 0 ? path_edges_processed / total_time_seconds
                                  : 0.0;
  }

  // ========== Output Methods ==========

  void print_summary(llvm::raw_ostream &os) const {
    os << "╔════════════════════════════════════════════════════════════╗\n";
    os << "║         IFDS/IDE Solver Statistics Summary                ║\n";
    os << "╠════════════════════════════════════════════════════════════╣\n";

    // Timing
    os << "║ TIMING                                                     ║\n";
    os << "║   Total time:              " << format_time(total_time_seconds)
       << "\n";
    os << "║   Initialization:          "
       << format_time(initialization_time_seconds) << "\n";
    os << "║   Tabulation:              "
       << format_time(tabulation_time_seconds) << "\n";
    if (value_computation_time_seconds > 0) {
      os << "║   Value computation (IDE): "
         << format_time(value_computation_time_seconds) << "\n";
    }
    os << "╠════════════════════════════════════════════════════════════╣\n";

    // Worklist
    os << "║ WORKLIST                                                   ║\n";
    os << "║   Path edges processed:    " << format_number(path_edges_processed)
       << "\n";
    os << "║   Path edges propagated:   "
       << format_number(path_edges_propagated) << "\n";
    os << "║   Total unique path edges: " << format_number(path_edges_total)
       << "\n";
    os << "║   Max worklist size:       " << format_number(max_worklist_size)
       << "\n";
    os << "║   Processing rate:         "
       << format_rate(path_edges_per_second()) << " edges/sec\n";
    os << "╠════════════════════════════════════════════════════════════╣\n";

    // Flow functions
    os << "║ FLOW FUNCTIONS                                             ║\n";
    os << "║   Normal flow:             "
       << format_number(normal_flow_applications) << "\n";
    os << "║   Call flow:               "
       << format_number(call_flow_applications) << "\n";
    os << "║   Return flow:             "
       << format_number(return_flow_applications) << "\n";
    os << "║   Call-to-return flow:     "
       << format_number(call_to_return_flow_applications) << "\n";
    if (flow_function_cache_hits + flow_function_cache_misses > 0) {
      os << "║   Cache hit rate:          "
         << format_percentage(flow_function_cache_hit_rate()) << "\n";
    }
    os << "╠════════════════════════════════════════════════════════════╣\n";

    // Edge functions (IDE only)
    if (edge_function_applications > 0) {
      os << "║ EDGE FUNCTIONS (IDE)                                       ║\n";
      os << "║   Applications:            "
         << format_number(edge_function_applications) << "\n";
      os << "║   Compositions:            "
         << format_number(edge_function_compositions) << "\n";
      os << "║   Joins:                   "
         << format_number(edge_function_joins) << "\n";
      os << "║   Identity functions:      "
         << format_number(identity_edge_functions) << "\n";
      if (edge_function_cache_hits + edge_function_cache_misses > 0) {
        os << "║   Cache hit rate:          "
           << format_percentage(edge_function_cache_hit_rate()) << "\n";
      }
      if (compose_cache_hits + compose_cache_misses > 0) {
        os << "║   Compose cache hit rate:  "
           << format_percentage(compose_cache_hit_rate()) << "\n";
      }
      if (join_cache_hits + join_cache_misses > 0) {
        os << "║   Join cache hit rate:     "
           << format_percentage(join_cache_hit_rate()) << "\n";
      }
      os << "╠════════════════════════════════════════════════════════════╣\n";
    }

    // Summaries
    if (summary_edges_total > 0) {
      os << "║ SUMMARY EDGES                                              ║\n";
      os << "║   Created:                 "
         << format_number(summary_edges_created) << "\n";
      os << "║   Reused:                  "
         << format_number(summary_edges_reused) << "\n";
      os << "║   Total:                   "
         << format_number(summary_edges_total) << "\n";
      os << "║   Reuse rate:              "
         << format_percentage(summary_reuse_rate()) << "\n";
      os << "╠════════════════════════════════════════════════════════════╣\n";
    }

    // Program
    os << "║ PROGRAM ANALYSIS                                           ║\n";
    os << "║   Functions analyzed:      " << format_number(functions_analyzed)
       << "\n";
    os << "║   Instructions analyzed:   "
       << format_number(instructions_analyzed) << "\n";
    os << "║   Call sites:              " << format_number(call_sites_analyzed)
       << "\n";
    if (indirect_call_sites > 0) {
      os << "║   Indirect calls:          "
         << format_number(indirect_call_sites) << "\n";
    }
    os << "╠════════════════════════════════════════════════════════════╣\n";

    // Results
    os << "║ RESULTS                                                    ║\n";
    os << "║   Total facts at entry:    " << format_number(facts_at_entry_total)
       << "\n";
    os << "║   Total facts at exit:     " << format_number(facts_at_exit_total)
       << "\n";
    os << "║   Avg facts per inst:      "
       << format_decimal(avg_facts_per_instruction()) << "\n";
    os << "║   Max facts at inst:       "
       << format_number(max_facts_at_instruction) << "\n";
    if (values_computed > 0) {
      os << "║   Values computed (IDE):   " << format_number(values_computed)
         << "\n";
    }

    os << "╚════════════════════════════════════════════════════════════╝\n";
  }

  void print_detailed(llvm::raw_ostream &os) const {
    print_summary(os);

    if (!facts_per_function.empty()) {
      os << "\n";
      os << "Per-Function Statistics:\n";
      os << "------------------------\n";
      for (const auto &pair : facts_per_function) {
        if (pair.first && pair.first->hasName()) {
          os << "  " << pair.first->getName() << ": " << pair.second
             << " facts";
          auto time_it = time_per_function.find(pair.first);
          if (time_it != time_per_function.end()) {
            os << ", " << format_time(time_it->second);
          }
          os << "\n";
        }
      }
    }
  }

  void reset() { *this = IFDSIDESolverStatistics(); }

  // Export to JSON for external analysis
  void export_json(llvm::raw_ostream &os) const {
    os << "{\n";
    os << "  \"timing\": {\n";
    os << "    \"total_seconds\": " << total_time_seconds << ",\n";
    os << "    \"initialization_seconds\": " << initialization_time_seconds
       << ",\n";
    os << "    \"tabulation_seconds\": " << tabulation_time_seconds << ",\n";
    os << "    \"value_computation_seconds\": "
       << value_computation_time_seconds << "\n";
    os << "  },\n";
    os << "  \"worklist\": {\n";
    os << "    \"path_edges_processed\": " << path_edges_processed << ",\n";
    os << "    \"path_edges_propagated\": " << path_edges_propagated << ",\n";
    os << "    \"path_edges_total\": " << path_edges_total << ",\n";
    os << "    \"max_worklist_size\": " << max_worklist_size << "\n";
    os << "  },\n";
    os << "  \"program\": {\n";
    os << "    \"functions_analyzed\": " << functions_analyzed << ",\n";
    os << "    \"instructions_analyzed\": " << instructions_analyzed << ",\n";
    os << "    \"call_sites_analyzed\": " << call_sites_analyzed << "\n";
    os << "  },\n";
    os << "  \"results\": {\n";
    os << "    \"facts_at_entry_total\": " << facts_at_entry_total << ",\n";
    os << "    \"facts_at_exit_total\": " << facts_at_exit_total << ",\n";
    os << "    \"values_computed\": " << values_computed << "\n";
    os << "  }\n";
    os << "}\n";
  }

private:
  static std::string format_time(double seconds) {
    if (seconds < 0.001) {
      return llvm::formatv("{0:F3} μs", seconds * 1e6).str();
    } else if (seconds < 1.0) {
      return llvm::formatv("{0:F3} ms", seconds * 1e3).str();
    } else if (seconds < 60.0) {
      return llvm::formatv("{0:F3} s", seconds).str();
    } else {
      int minutes = (int)(seconds / 60);
      double secs = seconds - minutes * 60;
      return llvm::formatv("{0}m {1:F3}s", minutes, secs).str();
    }
  }

  static std::string format_number(size_t num) {
    if (num < 1000) {
      return llvm::formatv("{0}", num).str();
    } else if (num < 1000000) {
      return llvm::formatv("{0:F2}K", static_cast<double>(num) / 1000.0).str();
    } else {
      return llvm::formatv("{0:F2}M", static_cast<double>(num) / 1000000.0)
          .str();
    }
  }

  static std::string format_percentage(double ratio) {
    return llvm::formatv("{0:F1}%", ratio * 100.0).str();
  }

  static std::string format_rate(double rate) {
    return format_number((size_t)rate);
  }

  static std::string format_decimal(double val) {
    return llvm::formatv("{0:F2}", val).str();
  }
};

} // namespace ifds

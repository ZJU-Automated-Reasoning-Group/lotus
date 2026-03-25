#ifndef LOTUS_DATAFLOW_MONO_SUPPORT_MONODEBUG_H_
#define LOTUS_DATAFLOW_MONO_SUPPORT_MONODEBUG_H_

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace mono {

// ============================================================================
// Debug Configuration
// ============================================================================

enum class DebugLevel {
  None = 0,    // No debug output
  Error = 1,   // Only errors
  Warning = 2, // Errors and warnings
  Info = 3,    // General information
  Verbose = 4, // Detailed tracing
  Debug = 5    // Maximum detail with worklist inspection
};

struct DebugConfig {
  DebugLevel level = DebugLevel::None;
  bool trace_worklist = false;
  bool trace_facts = false;
  bool trace_merging = false;
  bool collect_statistics = true;
  bool dump_final_results = false;
  size_t max_iterations_log = 1000; // Limit iterations to log

  static DebugConfig verbose() {
    DebugConfig cfg;
    cfg.level = DebugLevel::Verbose;
    cfg.trace_worklist = true;
    cfg.trace_facts = true;
    cfg.trace_merging = true;
    cfg.collect_statistics = true;
    cfg.dump_final_results = true;
    return cfg;
  }

  static DebugConfig minimal() {
    DebugConfig cfg;
    cfg.level = DebugLevel::Info;
    cfg.collect_statistics = true;
    cfg.dump_final_results = true;
    return cfg;
  }

  bool is_enabled(DebugLevel min_level) const {
    return static_cast<int>(level) >= static_cast<int>(min_level);
  }
};

// ============================================================================
// Solver Statistics
// ============================================================================

struct SolverStatistics {
  size_t iterations = 0;
  size_t worklist_max_size = 0;
  size_t worklist_total_pops = 0;
  size_t merge_operations = 0;
  size_t flow_function_calls = 0;
  size_t stabilization_checks = 0;
  size_t nodes_processed = 0;

  // Timing
  std::chrono::microseconds initialization_time{0};
  std::chrono::microseconds solving_time{0};
  std::chrono::microseconds total_time{0};

  void record_worklist_size(size_t size) {
    if (size > worklist_max_size) {
      worklist_max_size = size;
    }
  }

  void dump(llvm::raw_ostream &OS) const {
    OS << "========================================\n";
    OS << "Solver Statistics\n";
    OS << "========================================\n";
    OS << "Iterations: " << iterations << "\n";
    OS << "Worklist max size: " << worklist_max_size << "\n";
    OS << "Worklist total pops: " << worklist_total_pops << "\n";
    OS << "Merge operations: " << merge_operations << "\n";
    OS << "Flow function calls: " << flow_function_calls << "\n";
    OS << "Stabilization checks: " << stabilization_checks << "\n";
    OS << "Nodes processed: " << nodes_processed << "\n";
    OS << "----------------------------------------\n";
    OS << "Initialization time: " << initialization_time.count() << " us\n";
    OS << "Solving time: " << solving_time.count() << " us\n";
    OS << "Total time: " << total_time.count() << " us\n";
  }
};

// ============================================================================
// Tracing Utilities
// ============================================================================

template <typename N> class SolverTracer {
public:
  struct TraceEntry {
    enum Type {
      WorklistPop,
      FlowFunction,
      Merge,
      Stabilization,
      WorklistUpdate
    };

    Type type;
    N node;
    std::string description;
    std::chrono::steady_clock::time_point timestamp;
  };

  void add_trace(typename TraceEntry::Type type, N node,
                 const std::string &desc) {
    traces.push_back({type, node, desc, std::chrono::steady_clock::now()});
  }

  void dump_traces(llvm::raw_ostream &OS, size_t limit = 100) const {
    OS << "========================================\n";
    OS << "Solver Execution Trace (last " << limit << " entries)\n";
    OS << "========================================\n";

    size_t start = traces.size() > limit ? traces.size() - limit : 0;
    for (size_t i = start; i < traces.size(); ++i) {
      const auto &entry = traces[i];
      OS << "[" << i << "] ";
      switch (static_cast<int>(entry.type)) {
      case 0:
        OS << "POP";
        break;
      case 1:
        OS << "FLOW";
        break;
      case 2:
        OS << "MERGE";
        break;
      case 3:
        OS << "STABLE";
        break;
      case 4:
        OS << "UPDATE";
        break;
      default:
        OS << "UNKNOWN";
        break;
      }
      OS << ": " << entry.description << "\n";
    }
  }

  void clear() { traces.clear(); }

private:
  std::vector<TraceEntry> traces;
};

// ============================================================================
// DOT Graph Generation
// ============================================================================

template <typename N, typename Container> class DOTGraphGenerator {
public:
  struct NodeInfo {
    N node;
    Container facts;
    std::string label;
    std::string color = "black";
    std::string shape = "box";
  };

  struct EdgeInfo {
    N from;
    N to;
    std::string label;
    std::string color = "black";
  };

  void add_node(const NodeInfo &info) { nodes.push_back(info); }

  void add_edge(const EdgeInfo &info) { edges.push_back(info); }

  void emit_dot(llvm::raw_ostream &OS) const {
    OS << "digraph DataflowAnalysis {\n";
    OS << "  rankdir=TB;\n";
    OS << "  node [fontname=\"Helvetica\"];\n";
    OS << "\n";

    // Emit nodes
    for (const auto &node : nodes) {
      OS << "  \"" << get_node_id(node.node) << "\" [";
      OS << "label=\"" << escape_string(node.label) << "\",";
      OS << "color=\"" << node.color << "\",";
      OS << "shape=\"" << node.shape << "\"];\n";
    }

    OS << "\n";

    // Emit edges
    for (const auto &edge : edges) {
      OS << "  \"" << get_node_id(edge.from) << "\" -> \"";
      OS << get_node_id(edge.to) << "\" [";
      OS << "label=\"" << escape_string(edge.label) << "\",";
      OS << "color=\"" << edge.color << "\"];\n";
    }

    OS << "}\n";
  }

private:
  std::vector<NodeInfo> nodes;
  std::vector<EdgeInfo> edges;

  static std::string get_node_id(N node) {
    std::string id;
    llvm::raw_string_ostream rs(id);
    rs << "node_" << reinterpret_cast<uintptr_t>(node);
    return id;
  }

  static std::string escape_string(const std::string &str) {
    std::string result;
    for (char c : str) {
      switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
      }
    }
    return result;
  }
};

// ============================================================================
// Debug Macros
// ============================================================================

#define MONO_DEBUG_IF(LEVEL, CONFIG, CODE)                                     \
  do {                                                                         \
    if ((CONFIG).is_enabled(LEVEL)) {                                          \
      CODE;                                                                    \
    }                                                                          \
  } while (false)

#define MONO_TRACE_WORKLIST(OS, CONFIG, MSG)                                   \
  MONO_DEBUG_IF(mono::DebugLevel::Debug, CONFIG,                               \
                (OS) << "[WORKLIST] " << MSG << "\n";)

#define MONO_TRACE_FLOW(OS, CONFIG, MSG)                                       \
  MONO_DEBUG_IF(mono::DebugLevel::Verbose, CONFIG,                             \
                (OS) << "[FLOW] " << MSG << "\n";)

#define MONO_TRACE_MERGE(OS, CONFIG, MSG)                                      \
  MONO_DEBUG_IF(mono::DebugLevel::Verbose, CONFIG,                             \
                (OS) << "[MERGE] " << MSG << "\n";)

#define MONO_TRACE_FACTS(OS, CONFIG, MSG)                                      \
  MONO_DEBUG_IF(mono::DebugLevel::Verbose, CONFIG,                             \
                (OS) << "[FACTS] " << MSG << "\n";)

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_SUPPORT_MONODEBUG_H_

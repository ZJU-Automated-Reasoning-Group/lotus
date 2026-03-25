/*
 * Taint Configuration Parser
 *
 * Parser for taint specification files (.spec format)
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Support/raw_ostream.h>

// Taint specification for a single function argument or return value
struct TaintSpec {
  enum Location {
    ARG,       // Specific argument index
    AFTER_ARG, // All arguments after a specific index
    RET        // Return value
  };

  enum AccessMode {
    VALUE,          // Direct value (V)
    DIRECT_DEREF,   // Directly dereferenced memory (D)
    REACHABLE_DEREF // Reachable (transitive) memory (R)
  };

  enum TaintType {
    TAINTED,      // Tainted (T)
    UNINITIALIZED // Uninitialized (U)
  };

  Location location;
  AccessMode access_mode;
  TaintType taint_type;
  int arg_index; // For ARG or AFTER_ARG

  TaintSpec()
      : location(ARG), access_mode(VALUE), taint_type(TAINTED), arg_index(-1) {}
};

// Pipe specification for taint propagation
struct PipeSpec {
  TaintSpec from; // Source of taint
  TaintSpec to;   // Destination of taint
};

// Configuration for a single function
struct FunctionTaintConfig {
  std::vector<TaintSpec>
      source_specs;                  // How this function produces tainted data
  std::vector<TaintSpec> sink_specs; // Which arguments are checked as sinks
  std::vector<PipeSpec> pipe_specs;  // How taint flows through this function

  bool has_source_specs() const { return !source_specs.empty(); }
  bool has_sink_specs() const { return !sink_specs.empty(); }
  bool has_pipe_specs() const { return !pipe_specs.empty(); }
};

// Complete taint configuration
class TaintConfig {
public:
  std::unordered_set<std::string> sources;
  std::unordered_set<std::string> sinks;
  std::unordered_set<std::string> ignored;

  // Detailed specifications for each function
  std::unordered_map<std::string, FunctionTaintConfig> function_specs;

  bool is_source(const std::string &func) const { return sources.count(func); }
  bool is_sink(const std::string &func) const { return sinks.count(func); }
  bool is_ignored(const std::string &func) const { return ignored.count(func); }

  const FunctionTaintConfig *
  get_function_config(const std::string &func) const {
    auto it = function_specs.find(func);
    return (it != function_specs.end()) ? &it->second : nullptr;
  }

  void dump(llvm::raw_ostream &OS) const;
};

// Parser for taint config files
class TaintConfigParser {
public:
  static std::unique_ptr<TaintConfig> parse_file(const std::string &filename);
  static std::unique_ptr<TaintConfig>
  parse_file_quiet(const std::string &filename);
  static std::unique_ptr<TaintConfig> parse_string(const std::string &content);

private:
  static void parse_line(const std::string &line, TaintConfig &config);
  static bool parse_taint_spec(const std::vector<std::string> &tokens,
                               size_t start_idx, TaintSpec &spec);
  static std::vector<std::string> split(const std::string &str);
  static std::string trim(const std::string &str);
};

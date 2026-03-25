#ifndef CHECKER_REPORT_BUGREPORT_H
#define CHECKER_REPORT_BUGREPORT_H

#include "Checker/Report/BugTypes.h"

#include <map>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Value.h>

namespace llvm {
class Instruction;
} // namespace llvm

/**
 * Node tags categorize trace elements (inspired by Infer's node_tag)
 */
enum class NodeTag {
  NONE = 0,
  CONDITION_TRUE,
  CONDITION_FALSE,
  EXCEPTION,
  PROCEDURE_START,
  PROCEDURE_END,
  CALL_SITE,
  RETURN_SITE
};

/**
 * BugDiagStep describes a single step in the bug diagnostic trace.
 * A bug report consists of one or more diagnostic steps showing
 * how the bug manifests.
 *
 * Enhanced with Infer-inspired features:
 * - Trace levels for call hierarchy tracking
 * - Node tags for categorization
 * - Access information for additional context
 */
struct BugDiagStep {
  // The LLVM instruction or value for this diagnostic step
  llvm::Value *inst = nullptr;

  // Source file location
  std::string src_file;
  int src_line = 0;
  int src_column = 0;

  // Human-readable description of what happens at this step
  std::string tip;

  // Function containing this instruction
  std::string func_name;

  // LLVM IR representation
  std::string llvm_ir;

  // Variable/pointer name (if available from debug info or instruction name)
  std::string var_name;

  // Type information for the value
  std::string type_name;

  // Actual source code line (if available)
  std::string source_code;

  // Trace level: nesting level of procedure calls (0 = top level)
  // Inspired by Infer's lt_level
  int trace_level = 0;

  // Node tags: categorize the type of trace element
  // Inspired by Infer's node_tag
  std::vector<NodeTag> node_tags;

  // Access information: additional context about what is being accessed
  // Inspired by Infer's access field
  std::string access;

  // Node ID: identifier for the CFG node (if available)
  int node_id = -1;
};

/**
 * Extras metadata: extensible field for additional bug report information
 * Inspired by Infer's Jsonbug_t.extra
 */
struct BugReportExtras {
  // Suggestion for fixing the bug
  std::string suggestion;

  // Additional metadata as key-value pairs
  std::map<std::string, std::string> metadata;

  BugReportExtras() {}
};

/**
 * BugReport represents a complete bug with diagnostic trace.
 * Follows the shared pattern of reporting bugs as sequences of steps.
 * Enhanced with Infer-inspired features for better reporting.
 */
class BugReport {
public:
  BugReport(int bug_type_id)
      : bug_type_id(bug_type_id), dominated(false), valid(true),
        conf_score(100), session(0), extras(nullptr) {}

  ~BugReport() {
    for (BugDiagStep *step : trigger_steps)
      delete step;
    if (extras) {
      delete extras;
    }
  }

  // Add a diagnostic step to the trace
  void append_step(BugDiagStep *step) { trigger_steps.push_back(step); }

  // Enhanced version with trace level, node tags, and access information
  // This is the primary method - all checkers should use this
  void append_step(llvm::Value *inst, const std::string &tip,
                   int trace_level = 0, const std::vector<NodeTag> &tags = {},
                   const std::string &access = "");

  // Get the bug type ID
  int get_bug_type_id() const { return bug_type_id; }

  // Get all diagnostic steps
  const std::vector<BugDiagStep *> &get_steps() const { return trigger_steps; }

  // Dominated flag (for ranking/filtering)
  bool is_dominated() const { return dominated; }
  void set_dominated(bool val) { dominated = val; }

  // Valid flag (whether this report is considered valid)
  bool is_valid() const { return valid; }
  void set_valid(bool val) { valid = val; }

  // Confidence score [0-100]
  int get_conf_score() const { return conf_score; }
  void set_conf_score(int score) { conf_score = score; }

  // Session ID: distinguishes different analysis runs
  int get_session() const { return session; }
  void set_session(int s) { session = s; }

  // Extras metadata
  BugReportExtras *get_extras() const { return extras; }
  void set_extras(BugReportExtras *e) {
    if (extras)
      delete extras;
    extras = e;
  }

  // Helper to set suggestion
  void set_suggestion(const std::string &suggestion);

  // Helper to add metadata
  void add_metadata(const std::string &key, const std::string &value);

  // Export to JSON format
  void export_json(llvm::raw_ostream &OS) const;

  // Compute hash for deduplication (based on location or trace)
  size_t compute_hash(bool use_trace = false) const;

private:
  int bug_type_id;
  std::vector<BugDiagStep *> trigger_steps;
  bool dominated;
  bool valid;
  int conf_score;
  int session;             // Session ID for distinguishing analysis runs
  BugReportExtras *extras; // Optional extensible metadata
};

// Print a formatted bug report with debug information
void printBugReport(const llvm::Instruction *BugInst,
                    const std::string &BugType,
                    const llvm::Value *RelatedValue = nullptr);

#endif // CHECKER_REPORT_BUGREPORT_H

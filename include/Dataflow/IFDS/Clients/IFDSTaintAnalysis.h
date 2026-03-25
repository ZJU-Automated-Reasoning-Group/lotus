/*
 * Interprocedural Taint Analysis using IFDS
 *
 * This implements a concrete taint analysis as an example of using the IFDS
 * framework.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Solvers/IFDSSolver.h"

#include <string>
#include <unordered_set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// Taint Fact Definition
// ============================================================================

class TaintFact {
public:
  enum Type {
    ZERO,            // Lambda fact (always holds)
    TAINTED_VAR,     // SSA value is tainted
    TAINTED_MEMORY,  // Memory location is tainted
    TAINTED_FIELD,   // Specific field of aggregate is tainted
    TAINTED_GLOBAL,  // Global variable is tainted
    TAINTED_IMPLICIT // Implicit flow (control dependence)
  };

  enum TaintKind {
    TAINT_UNKNOWN = 0, // Unknown/generic taint
    TAINT_USER_INPUT,  // From user input (stdin, argv, etc.)
    TAINT_FILE,        // From file read
    TAINT_NETWORK,     // From network
    TAINT_ENVIRONMENT, // From environment variables
    TAINT_SANITIZED    // Was tainted but sanitized (partial)
  };

private:
  Type m_type;
  const llvm::Value *m_value;             // For variables
  const llvm::Value *m_memory_location;   // For memory locations
  const llvm::Instruction *m_source_inst; // Where this taint originated
  int m_field_index;      // For field-sensitive tracking (-1 = all fields)
  TaintKind m_taint_kind; // Classification of taint source

public:
  TaintFact();

  static TaintFact zero();
  static TaintFact tainted_var(const llvm::Value *v,
                               const llvm::Instruction *source = nullptr);
  static TaintFact tainted_memory(const llvm::Value *loc,
                                  const llvm::Instruction *source = nullptr);
  static TaintFact tainted_field(const llvm::Value *base, int field_idx,
                                 const llvm::Instruction *source = nullptr);
  static TaintFact tainted_global(const llvm::GlobalVariable *gv,
                                  const llvm::Instruction *source = nullptr);
  static TaintFact tainted_implicit(const llvm::Value *control_val,
                                    const llvm::Instruction *source = nullptr);

  bool operator==(const TaintFact &other) const;
  bool operator<(const TaintFact &other) const;
  bool operator!=(const TaintFact &other) const;

  Type get_type() const;
  const llvm::Value *get_value() const;
  const llvm::Value *get_memory_location() const;
  const llvm::Instruction *get_source() const;
  int get_field_index() const;
  TaintKind get_taint_kind() const;

  bool is_zero() const;
  bool is_tainted_var() const;
  bool is_tainted_memory() const;
  bool is_tainted_field() const;
  bool is_tainted_global() const;
  bool is_tainted_implicit() const;

  // Create a new fact with the same taint but different source
  TaintFact with_source(const llvm::Instruction *source) const;
  TaintFact with_kind(TaintKind kind) const;

  friend std::ostream &operator<<(std::ostream &os, const TaintFact &fact);
};

} // namespace ifds

// Hash function for TaintFact
namespace std {
template <> struct hash<ifds::TaintFact> {
  size_t operator()(const ifds::TaintFact &fact) const;
};
} // namespace std

namespace ifds {

// ============================================================================
// Interprocedural Taint Analysis using IFDS
// ============================================================================

class TaintAnalysis : public DefaultAliasAwareIFDSProblem<TaintFact> {
public:
  // Configuration options for analysis precision
  struct Config {
    bool track_implicit_flows = false; // Track control-dependence based taint
    bool field_sensitive = true;       // Track individual struct fields
    bool track_globals = true;         // Track global variables
    bool track_arrays = true;          // Track array element taint
    bool use_sanitizers = true;        // Apply sanitizer specifications
    bool strict_sanitization = false; // Require exact type match for sanitizers
  };

private:
  std::unordered_set<std::string> m_source_functions;
  std::unordered_set<std::string> m_sink_functions;
  std::unordered_set<std::string> m_sanitizer_functions;
  Config m_config;

  // Track control flow for implicit taint
  mutable std::unordered_set<const llvm::BasicBlock *> m_tainted_branches;

public:
  TaintAnalysis();
  explicit TaintAnalysis(const Config &config);

  // Configuration
  void set_config(const Config &config) { m_config = config; }
  const Config &get_config() const { return m_config; }

  // IFDS interface implementation
  TaintFact zero_fact() const override;
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const TaintFact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const TaintFact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const TaintFact &exit_fact,
                      const TaintFact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const TaintFact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

  // Override source/sink detection
  bool is_source(const llvm::Instruction *inst) const override;
  bool is_sink(const llvm::Instruction *inst) const override;

  // Add custom source/sink/sanitizer functions
  void add_source_function(const std::string &func_name);
  void add_sink_function(const std::string &func_name);
  void add_sanitizer_function(const std::string &func_name);

  // Check if a function sanitizes taint
  bool is_sanitizer(const llvm::Instruction *inst) const;

  // Vulnerability detection and reporting
  void report_vulnerabilities(const IFDSSolver<TaintAnalysis> &solver,
                              llvm::raw_ostream &OS,
                              size_t max_vulnerabilities = 10) const;

  // Exposed for reporting utilities
  struct TaintPath {
    std::vector<const llvm::Instruction *> sources;
    std::vector<const llvm::Function *> intermediate_functions;
  };

  // Tracing method for reconstructing taint propagation paths
  TaintPath
  trace_taint_sources_summary_based(const IFDSSolver<TaintAnalysis> &solver,
                                    const llvm::CallBase *sink_call,
                                    const TaintFact &tainted_fact) const;

  bool is_argument_tainted(const llvm::Value *arg, const TaintFact &fact) const;
  std::string format_tainted_arg(unsigned arg_index, const TaintFact &fact,
                                 const llvm::CallBase *call) const;
  void analyze_tainted_arguments(const llvm::CallBase *call,
                                 const FactSet &facts,
                                 std::string &tainted_args) const;
  void output_vulnerability_report(
      llvm::raw_ostream &OS, size_t vuln_num, const std::string &func_name,
      const llvm::CallBase *call, const std::string &tainted_args,
      const std::vector<const llvm::Instruction *> &all_sources,
      const std::vector<const llvm::Function *> &propagation_path,
      size_t max_vulnerabilities) const;

  // Helper for boundary-only tracing
  bool comes_before(const llvm::Instruction *first,
                    const llvm::Instruction *second) const;

private:
  bool kills_fact(const llvm::CallBase *call, const TaintFact &fact) const;
  bool taint_may_alias(const llvm::Value *v1, const llvm::Value *v2) const;

  // Internal helpers that need access to alias analysis utilities
  void handle_source_function_specs(const llvm::CallBase *call,
                                    FactSet &result) const;
  void handle_pipe_specifications(const llvm::CallBase *call,
                                  const TaintFact &fact, FactSet &result) const;
};

} // namespace ifds

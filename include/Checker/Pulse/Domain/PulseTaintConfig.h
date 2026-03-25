#ifndef CHECKER_PULSE_PULSETAINTCONFIG_H
#define CHECKER_PULSE_PULSETAINTCONFIG_H

#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace pulse {

/**
 * TaintKind: category of taint source/sink
 * Aligned with Infer's TaintConfig.Kind
 */
class TaintKind {
private:
  std::string name_;
  bool is_data_flow_only_; // True if this kind is only for data flow tracking

public:
  TaintKind(const std::string &name, bool is_data_flow_only = false)
      : name_(name), is_data_flow_only_(is_data_flow_only) {}

  const std::string &getName() const { return name_; }
  bool isDataFlowOnly() const { return is_data_flow_only_; }

  bool operator<(const TaintKind &other) const { return name_ < other.name_; }
  bool operator==(const TaintKind &other) const { return name_ == other.name_; }
  bool operator!=(const TaintKind &other) const { return name_ != other.name_; }

  // Common taint kinds
  static TaintKind UserInput() { return TaintKind("UserInput"); }
  static TaintKind Network() { return TaintKind("Network"); }
  static TaintKind FileSystem() { return TaintKind("FileSystem"); }
  static TaintKind Environment() { return TaintKind("Environment"); }
  static TaintKind Sensitive() { return TaintKind("Sensitive"); }
  static TaintKind Unknown() { return TaintKind("Unknown"); }
};

/**
 * ProcedureMatcher: matches procedures for taint sources/sinks
 * Aligned with Infer's TaintConfig.Unit.procedure_matcher
 */
class ProcedureMatcher {
public:
  enum class Type {
    ProcedureName,       // Exact name match
    ProcedureNameRegex,  // Regex match on name
    ClassNameRegex,      // Regex match on class name
    ClassAndMethodNames, // Match class and method names
    BuiltinName          // Builtin function name
  };

private:
  Type type_;
  std::string name_;
  std::regex name_regex_;
  std::vector<std::string> class_names_;
  std::vector<std::string> method_names_;
  std::vector<std::string> exclude_in_;
  std::vector<std::string> exclude_names_;

public:
  ProcedureMatcher(Type t, const std::string &name) : type_(t), name_(name) {
    if (t == Type::ProcedureNameRegex || t == Type::ClassNameRegex) {
      name_regex_ = std::regex(name);
    }
  }

  bool matches(const llvm::Function *func) const;

  Type getType() const { return type_; }
  const std::string &getName() const { return name_; }
};

/**
 * ProcedureTarget: specifies which part of a procedure is tainted
 * Aligned with Infer's TaintConfig.Target.procedure_target
 */
enum class ProcedureTarget {
  ReturnValue,             // Return value is tainted
  AllArguments,            // All arguments are tainted
  ArgumentPositions,       // Specific argument positions
  InstanceReference,       // Instance reference (this/self)
  AllArgumentsButPositions // All arguments except specified positions
};

/**
 * ProcedureUnit: defines a taint source/sink/sanitizer for a procedure
 */
struct ProcedureUnit {
  ProcedureMatcher matcher;
  std::vector<TaintKind> kinds;
  ProcedureTarget target;
  std::vector<int> argument_positions; // For ArgumentPositions target

  ProcedureUnit(const ProcedureMatcher &m, const std::vector<TaintKind> &k,
                ProcedureTarget t)
      : matcher(m), kinds(k), target(t) {}
};

/**
 * SinkPolicy: defines when a taint flow should be reported
 * Aligned with Infer's TaintConfig.SinkPolicy
 */
struct SinkPolicy {
  std::vector<TaintKind> source_kinds; // Which source kinds trigger this policy
  std::vector<TaintKind> sanitizer_kinds; // Which sanitizers prevent reporting
  std::string description;
  int policy_id;
  std::string privacy_effect;
  std::string report_as_issue_type;
  std::string report_as_category;

  SinkPolicy(int id, const std::string &desc)
      : description(desc), policy_id(id) {}
};

/**
 * TaintConfig: configuration for taint analysis
 * Aligned with Infer's PulseTaintConfig
 */
class TaintConfig {
private:
  // Source matchers
  std::vector<ProcedureUnit> source_procedures_;
  std::vector<ProcedureUnit> source_blocks_;

  // Sink matchers
  std::vector<ProcedureUnit> sink_procedures_;

  // Sanitizer matchers
  std::vector<ProcedureUnit> sanitizer_procedures_;

  // Propagator matchers (functions that propagate taint)
  std::vector<ProcedureUnit> propagator_procedures_;

  // Sink policies: map from sink kind to policies
  std::map<TaintKind, std::vector<SinkPolicy>> sink_policies_;

  // Allocation sources: class names that create taint
  std::map<std::string, std::vector<TaintKind>> allocation_sources_;

  static TaintConfig *default_config_;

public:
  TaintConfig() = default;

  // Add matchers
  void addSourceProcedure(const ProcedureUnit &unit) {
    source_procedures_.push_back(unit);
  }
  void addSinkProcedure(const ProcedureUnit &unit) {
    sink_procedures_.push_back(unit);
  }
  void addSanitizerProcedure(const ProcedureUnit &unit) {
    sanitizer_procedures_.push_back(unit);
  }
  void addPropagatorProcedure(const ProcedureUnit &unit) {
    propagator_procedures_.push_back(unit);
  }

  // Add sink policy
  void addSinkPolicy(const TaintKind &sink_kind, const SinkPolicy &policy) {
    sink_policies_[sink_kind].push_back(policy);
  }

  // Add allocation source
  void addAllocationSource(const std::string &class_name,
                           const std::vector<TaintKind> &kinds) {
    allocation_sources_[class_name] = kinds;
  }

  // Check if function is a source
  std::vector<TaintKind> isSource(const llvm::Function *func) const;

  // Check if function is a sink
  bool isSink(const llvm::Function *func,
              std::vector<TaintKind> &sink_kinds) const;

  // Check if function is a sanitizer
  std::vector<TaintKind> isSanitizer(const llvm::Function *func) const;

  // Check if function is a propagator
  bool isPropagator(const llvm::Function *func) const;

  // Get sink policies for a sink kind
  const std::vector<SinkPolicy> &
  getSinkPolicies(const TaintKind &sink_kind) const;

  // Get allocation source kinds
  std::vector<TaintKind>
  getAllocationSourceKinds(const std::string &class_name) const;

  // Get default configuration (with common sources/sinks)
  static const TaintConfig &getDefault();

  // Initialize default configuration
  static void initializeDefault();
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSETAINTCONFIG_H

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace ifds {

// ============================================================================
// Linear Constant Analysis Fact
// ============================================================================

using LCAFact = const llvm::Value *;

// ============================================================================
// Lattice Domain for Linear Constant Analysis
// ============================================================================

struct LCALattice {
  enum Kind {
    TOP,   // Unknown value (could be any constant)
    CONST, // Known constant value
    BOTTOM // Not a constant / multiple possible values
  };

  Kind kind;
  int64_t value;

  LCALattice() : kind(TOP), value(0) {}
  explicit LCALattice(Kind k) : kind(k), value(0) {}
  LCALattice(int64_t v) : kind(CONST), value(v) {}

  static LCALattice top() { return LCALattice(TOP); }
  static LCALattice bottom() { return LCALattice(BOTTOM); }
  static LCALattice constant(int64_t v) { return LCALattice(v); }

  bool is_top() const { return kind == TOP; }
  bool is_bottom() const { return kind == BOTTOM; }
  bool is_constant() const { return kind == CONST; }
  int64_t get_constant() const { return value; }

  bool operator==(const LCALattice &other) const {
    if (kind != other.kind)
      return false;
    if (kind == CONST)
      return value == other.value;
    return true;
  }

  bool operator!=(const LCALattice &other) const { return !(*this == other); }

  // Join (least upper bound) operation
  LCALattice join(const LCALattice &other) const {
    if (is_bottom())
      return other;
    if (other.is_bottom())
      return *this;
    if (is_top() || other.is_top())
      return top();
    if (kind == CONST && other.kind == CONST && value == other.value) {
      return *this;
    }
    return bottom();
  }

  std::string to_string() const {
    switch (kind) {
    case TOP:
      return "TOP";
    case BOTTOM:
      return "BOTTOM";
    case CONST:
      return std::to_string(value);
    }
    return "INVALID";
  }
};

// ============================================================================
// Linear Constant Analysis Result Structure
// ============================================================================

struct LCAResult {
  unsigned line_number = 0;
  std::string source_node;
  std::map<std::string, LCALattice> variable_values;
  std::vector<const llvm::Instruction *> ir_trace;

  void print(llvm::raw_ostream &OS) const;
  bool operator==(const LCAResult &other) const;
};

// ============================================================================
// Linear Constant Analysis (IDE)
// ============================================================================

class IDELinearConstantAnalysis
    : public DefaultNoAliasIDEProblem<LCAFact, LCALattice> {
public:
  using Fact = LCAFact;
  using Value = LCALattice;
  using FactSet = typename DefaultNoAliasIDEProblem<Fact, Value>::FactSet;
  using EdgeFunction =
      typename DefaultNoAliasIDEProblem<Fact, Value>::EdgeFunction;

  IDELinearConstantAnalysis();

  // IFDS interface - flow functions
  Fact zero_fact() const override;
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ, const Fact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override;
  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  // Value domain operations
  Value top_value() const override { return Value::top(); }
  Value bottom_value() const override { return Value::bottom(); }
  Value join(const Value &v1, const Value &v2) const override;

  // Edge functions
  EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                    const llvm::Instruction *succ,
                                    const Fact &src_fact,
                                    const Fact &tgt_fact) override;
  EdgeFunction call_edge_function(const llvm::CallBase *call,
                                  const llvm::Function *callee,
                                  const Fact &src_fact,
                                  const Fact &tgt_fact) override;
  EdgeFunction return_edge_function(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const llvm::Instruction *exit_inst,
                                    const llvm::Instruction *return_site,
                                    const Fact &exit_fact,
                                    const Fact &ret_fact) override;
  EdgeFunction call_to_return_edge_function(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees, const Fact &src_fact,
      const Fact &tgt_fact) override;

  // Result processing
  std::map<std::string, std::map<unsigned, LCAResult>> get_lca_results(
      const std::unordered_map<const llvm::Instruction *,
                               std::unordered_map<Fact, Value>> &all_values)
      const;

  void emit_text_report(
      const std::unordered_map<const llvm::Instruction *,
                               std::unordered_map<Fact, Value>> &all_values,
      llvm::raw_ostream &OS = llvm::outs()) const;

private:
  // Helper methods
  static bool defines_value(const llvm::Instruction *inst);
  static const llvm::Value *get_defined_value(const llvm::Instruction *inst);
  static llvm::Optional<int64_t> as_const(const llvm::Value *val);
  static llvm::Optional<int64_t> apply_binop(unsigned opcode, int64_t lhs,
                                             int64_t rhs);
  static bool is_linear_operation(const llvm::Instruction *inst);
  static llvm::Optional<int64_t>
  compute_linear_transformation(const llvm::Instruction *inst,
                                int64_t input_val);

  // Edge function helpers
  EdgeFunction create_identity() const;
  EdgeFunction create_constant(int64_t val) const;
  EdgeFunction create_linear(int64_t multiplier, int64_t offset) const;
  EdgeFunction create_bottom() const;
};

} // namespace ifds

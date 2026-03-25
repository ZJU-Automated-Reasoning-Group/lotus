/*
 * IFDS Uninitialized Variables Analysis
 *
 * This analysis detects uses of uninitialized variables.
 * It tracks which memory locations have been initialized through stores
 * and reports uses of values that may be uninitialized.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <functional>
#include <map>
#include <set>

#include <llvm/IR/Instructions.h>

namespace ifds {

// ============================================================================
// Uninitialized Variables Fact
// ============================================================================

struct UninitVarFact {
  enum Type {
    ZERO,          // Lambda fact
    UNINITIALIZED, // Memory location is uninitialized
    INITIALIZED    // Memory location has been initialized
  };

  Type type;
  const llvm::Value *value; // The memory location or value

  UninitVarFact() : type(ZERO), value(nullptr) {}
  UninitVarFact(Type t, const llvm::Value *v) : type(t), value(v) {}

  static UninitVarFact zero() { return UninitVarFact(ZERO, nullptr); }
  static UninitVarFact uninitialized(const llvm::Value *v) {
    return UninitVarFact(UNINITIALIZED, v);
  }
  static UninitVarFact initialized(const llvm::Value *v) {
    return UninitVarFact(INITIALIZED, v);
  }

  bool operator==(const UninitVarFact &other) const {
    return type == other.type && value == other.value;
  }
  bool operator!=(const UninitVarFact &other) const {
    return !(*this == other);
  }

  bool operator<(const UninitVarFact &other) const {
    if (type != other.type)
      return type < other.type;
    return std::less<const llvm::Value *>{}(value, other.value);
  }

  bool is_zero() const { return type == ZERO; }
  bool is_uninitialized() const { return type == UNINITIALIZED; }
  bool is_initialized() const { return type == INITIALIZED; }
};

// Specialize fact_less so PathEdge<UninitVarFact> and
// SummaryEdge<UninitVarFact> can be used in std::set without requiring
// operator< visible at template instantiation. Uses std::less for pointer
// comparison.
template <>
inline bool fact_less<UninitVarFact>(const UninitVarFact &a,
                                     const UninitVarFact &b) {
  if (a.type != b.type)
    return a.type < b.type;
  return std::less<const llvm::Value *>{}(a.value, b.value);
}

} // namespace ifds

namespace std {
template <> struct hash<ifds::UninitVarFact> {
  size_t operator()(const ifds::UninitVarFact &fact) const {
    // FNV-1a-style mixing to avoid XOR-shift collisions on aligned hashes.
    size_t h = 14695981039346656037ULL;
    h ^= std::hash<int>{}(static_cast<int>(fact.type));
    h *= 1099511628211ULL;
    h ^= std::hash<const llvm::Value *>{}(fact.value);
    h *= 1099511628211ULL;
    return h;
  }
};
} // namespace std

namespace ifds {

// ============================================================================
// Uninitialized Variables Analysis
// ============================================================================

class UninitializedVariablesAnalysis
    : public DefaultAliasAwareIFDSProblem<UninitVarFact> {
public:
  struct UninitResult {
    const llvm::Instruction *use_site;
    const llvm::Value *uninitialized_value;
    std::set<const llvm::Instruction *> trace;

    UninitResult(const llvm::Instruction *use, const llvm::Value *val)
        : use_site(use), uninitialized_value(val) {}
  };

private:
  std::map<const llvm::Instruction *, std::set<const llvm::Value *>> undef_uses;
  std::set<const llvm::Value *> initialized_locations;

public:
  UninitializedVariablesAnalysis();

  // IFDS interface implementation
  UninitVarFact zero_fact() const override;
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const UninitVarFact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const UninitVarFact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const UninitVarFact &exit_fact,
                      const UninitVarFact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const UninitVarFact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

  // Source detection - uninitialized variable uses are "sources" of bugs
  bool is_source(const llvm::Instruction *inst) const override;

  // Results
  std::vector<UninitResult> get_results() const;
  void emit_report(llvm::raw_ostream &OS = llvm::outs()) const;

private:
  bool is_initialized(const llvm::Value *val) const;
  void mark_initialized(const llvm::Value *val);
  bool may_be_uninitialized(const llvm::Value *val, const FactSet &facts) const;
};

} // namespace ifds

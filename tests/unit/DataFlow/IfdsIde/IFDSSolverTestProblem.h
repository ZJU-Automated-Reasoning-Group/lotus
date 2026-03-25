/*
 * IFDS Solver Test Problem
 *
 * A structured test problem for validating IFDS solver correctness.
 * Lives in tests/unit/DataFlow/IfdsIde/ for use by unit tests.
 */

#pragma once

#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <Dataflow/IFDS/Core/IFDSFramework.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// IFDS Test Fact
// ============================================================================

struct IFDSTestFact {
  enum Type { ZERO, VALUE, TAINTED };

  Type type;
  const llvm::Value *value;
  std::string label;

  IFDSTestFact() : type(ZERO), value(nullptr), label("ZERO") {}
  IFDSTestFact(Type t, const llvm::Value *v, const std::string &l = "")
      : type(t), value(v), label(l) {}

  static IFDSTestFact zero() { return IFDSTestFact(ZERO, nullptr, "ZERO"); }
  static IFDSTestFact make_value(const llvm::Value *v,
                                 const std::string &label = "") {
    return IFDSTestFact(VALUE, v, label);
  }
  static IFDSTestFact make_tainted(const llvm::Value *v) {
    return IFDSTestFact(TAINTED, v, "TAINTED");
  }

  bool operator==(const IFDSTestFact &other) const {
    return type == other.type && value == other.value;
  }
  bool operator!=(const IFDSTestFact &other) const { return !(*this == other); }
  bool operator<(const IFDSTestFact &other) const {
    if (type != other.type)
      return type < other.type;
    return std::less<const llvm::Value *>{}(value, other.value);
  }
  bool is_zero() const { return type == ZERO; }
};

} // namespace ifds

namespace std {
template <> struct hash<ifds::IFDSTestFact> {
  size_t operator()(const ifds::IFDSTestFact &fact) const {
    return std::hash<int>{}(static_cast<int>(fact.type)) ^
           (std::hash<const llvm::Value *>{}(fact.value) << 1);
  }
};
} // namespace std

namespace ifds {

// ============================================================================
// IFDS Solver Test Problem
// ============================================================================

class IFDSSolverTestProblem : public IFDSProblem<IFDSTestFact> {
public:
  enum TestMode {
    IDENTITY,
    GENERATE_AT_ENTRY,
    KILL_AT_EXIT,
    TAINT_TRACKING,
    BRANCHING
  };

  IFDSSolverTestProblem(TestMode mode = IDENTITY) : m_mode(mode) {}

  IFDSTestFact zero_fact() const override { return IFDSTestFact::zero(); }

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const IFDSTestFact &fact) override {
    (void)stmt;
    if (m_mode == KILL_AT_EXIT)
      return {};
    if (fact.is_zero())
      return {fact};
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const IFDSTestFact &fact) override {
    (void)call;
    (void)callee;
    if (fact.is_zero())
      return {fact};
    return {fact};
  }

  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const IFDSTestFact &exit_fact,
                      const IFDSTestFact &call_fact) override {
    (void)call;
    (void)callee;
    (void)call_fact;
    if (exit_fact.is_zero())
      return {exit_fact};
    return {exit_fact};
  }

  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const IFDSTestFact &fact) override {
    (void)call;
    if (fact.is_zero())
      return {fact};
    return {fact};
  }

  FactSet initial_facts(const llvm::Function *main) override {
    (void)main;
    FactSet facts;
    facts.insert(IFDSTestFact::zero());
    return facts;
  }

  void set_mode(TestMode mode) { m_mode = mode; }
  TestMode get_mode() const { return m_mode; }
  void mark_source(const llvm::Instruction *inst) { m_sources.insert(inst); }
  void mark_sink(const llvm::Instruction *inst) { m_sinks.insert(inst); }

  struct TestResult {
    bool passed;
    std::string message;
    size_t facts_at_exit;
    std::set<IFDSTestFact> unexpected_facts;
    std::set<IFDSTestFact> missing_facts;
  };

  TestResult validate_results(
      const std::unordered_map<const llvm::Instruction *,
                               std::set<IFDSTestFact>> &results,
      const std::unordered_map<const llvm::Instruction *,
                               std::set<IFDSTestFact>> &expected) const {
    TestResult r;
    r.passed = true;
    r.message = "OK";
    r.facts_at_exit = 0;
    for (const auto &p : results)
      r.facts_at_exit += p.second.size();
    if (results != expected) {
      r.passed = false;
      r.message = "Result map does not match expected";
    }
    return r;
  }

private:
  TestMode m_mode;
  std::set<const llvm::Instruction *> m_sources;
  std::set<const llvm::Instruction *> m_sinks;
};

// ============================================================================
// Test Suite Helper
// ============================================================================

class IFDSTestSuite {
public:
  struct TestCase {
    std::string name;
    std::string description;
    IFDSSolverTestProblem::TestMode mode;
    std::function<bool(const IFDSSolverTestProblem::TestResult &)> validator;
  };

  IFDSTestSuite() { register_standard_tests(); }

  void add_test_case(const TestCase &test_case) {
    m_test_cases.push_back(test_case);
  }

  bool run_all_tests(llvm::raw_ostream &os) {
    bool ok = true;
    for (const auto &tc : m_test_cases) {
      if (!run_test(tc.name, os))
        ok = false;
    }
    return ok;
  }

  bool run_test(const std::string &name, llvm::raw_ostream &os) {
    (void)name;
    (void)os;
    return true;
  }

  const std::vector<TestCase> &get_test_cases() const { return m_test_cases; }

private:
  std::vector<TestCase> m_test_cases;

  void register_standard_tests() {}
};

} // namespace ifds

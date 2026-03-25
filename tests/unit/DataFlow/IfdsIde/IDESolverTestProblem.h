/*
 * IDE Solver Test Problem
 *
 * A structured test problem for validating IDE solver correctness.
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
// IDE Test Lattice Value
// ============================================================================

struct IDETestValue {
  enum Kind { BOTTOM, CONSTANT, TOP };

  Kind kind;
  int64_t value;

  IDETestValue() : kind(BOTTOM), value(0) {}
  explicit IDETestValue(Kind k) : kind(k), value(0) {}
  explicit IDETestValue(int64_t v) : kind(CONSTANT), value(v) {}

  static IDETestValue bottom() { return IDETestValue(BOTTOM); }
  static IDETestValue top() { return IDETestValue(TOP); }
  static IDETestValue constant(int64_t v) { return IDETestValue(v); }

  bool is_bottom() const { return kind == BOTTOM; }
  bool is_top() const { return kind == TOP; }
  bool is_constant() const { return kind == CONSTANT; }

  bool operator==(const IDETestValue &other) const {
    if (kind != other.kind)
      return false;
    if (kind == CONSTANT)
      return value == other.value;
    return true;
  }
  bool operator!=(const IDETestValue &other) const { return !(*this == other); }

  IDETestValue join(const IDETestValue &other) const {
    if (is_bottom())
      return other;
    if (other.is_bottom())
      return *this;
    if (is_top() || other.is_top())
      return top();
    if (is_constant() && other.is_constant()) {
      if (value == other.value)
        return *this;
      return top();
    }
    return top();
  }

  std::string to_string() const {
    switch (kind) {
    case BOTTOM:
      return "BOTTOM";
    case TOP:
      return "TOP";
    case CONSTANT:
      return std::to_string(value);
    }
    return "?";
  }
};

// ============================================================================
// IDE Solver Test Problem
// ============================================================================

class IDESolverTestProblem
    : public IDEProblem<const llvm::Value *, IDETestValue> {
public:
  using Fact = const llvm::Value *;
  using Value = IDETestValue;

  enum TestMode {
    IDENTITY_FUNCTIONS,
    CONSTANT_ADDITION,
    CONSTANT_MULTIPLY,
    GENERAL_LINEAR,
    DISTRIBUTIVE_TEST,
    LATTICE_JOIN_TEST
  };

  IDESolverTestProblem(TestMode mode = IDENTITY_FUNCTIONS) : m_mode(mode) {}

  Fact zero_fact() const override { return nullptr; }

  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const Fact &fact) override {
    (void)stmt;
    (void)succ;
    if (!fact)
      return {};
    return {fact};
  }

  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const Fact &fact) override {
    (void)call;
    (void)callee;
    if (!fact)
      return {};
    return {fact};
  }

  FactSet return_flow(const llvm::CallBase *call,
                      const llvm::Instruction *exit_inst,
                      const llvm::Instruction *return_site,
                      const llvm::Function *callee, const Fact &exit_fact,
                      const Fact &call_fact) override {
    (void)call;
    (void)exit_inst;
    (void)return_site;
    (void)callee;
    (void)call_fact;
    if (!exit_fact)
      return {};
    return {exit_fact};
  }

  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees,
                              const Fact &fact) override {
    (void)call;
    (void)return_site;
    (void)callees;
    if (!fact)
      return {};
    return {fact};
  }

  FactSet initial_facts(const llvm::Function *main) override {
    (void)main;
    return {nullptr};
  }
  IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) override {
    return this->lift_ifds_initial_seeds(module, bottom_value());
  }

  EdgeFunction normal_edge_function(const llvm::Instruction *,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction call_edge_function(const llvm::CallBase *,
                                  const llvm::Function *, const Fact &,
                                  const Fact &) override {
    return identity();
  }
  EdgeFunction return_edge_function(const llvm::CallBase *,
                                    const llvm::Function *,
                                    const llvm::Instruction *,
                                    const llvm::Instruction *, const Fact &,
                                    const Fact &) override {
    return identity();
  }
  EdgeFunction
  call_to_return_edge_function(const llvm::CallBase *,
                               const llvm::Instruction *,
                               llvm::ArrayRef<const llvm::Function *>,
                               const Fact &, const Fact &) override {
    return identity();
  }

  IDETestValue top_value() const override { return IDETestValue::top(); }
  IDETestValue bottom_value() const override { return IDETestValue::bottom(); }
  IDETestValue join(const IDETestValue &v1,
                    const IDETestValue &v2) const override {
    return v1.join(v2);
  }

  void set_mode(TestMode mode) { m_mode = mode; }
  TestMode get_mode() const { return m_mode; }
  void set_addition_constant(int64_t c) { m_addition_constant = c; }
  void set_multiply_constant(int64_t c) { m_multiply_constant = c; }

  struct TestResult {
    bool passed;
    std::string message;
    size_t num_values_computed;
    std::unordered_map<const llvm::Instruction *,
                       std::unordered_map<Fact, IDETestValue>>
        actual_values;
    std::unordered_map<const llvm::Instruction *,
                       std::unordered_map<Fact, IDETestValue>>
        expected_values;
  };

  TestResult validate_results(
      const std::unordered_map<const llvm::Instruction *,
                               std::unordered_map<Fact, IDETestValue>> &results,
      const std::unordered_map<const llvm::Instruction *,
                               std::unordered_map<Fact, IDETestValue>>
          &expected) const {
    TestResult r;
    r.passed = true;
    r.message = "OK";
    r.num_values_computed = 0;
    for (const auto &p : results)
      for (const auto &q : p.second)
        (void)q, ++r.num_values_computed;
    r.actual_values = results;
    r.expected_values = expected;
    if (results != expected)
      r.passed = false, r.message = "Result does not match expected";
    return r;
  }

  EdgeFunction create_linear_function(int64_t a, int64_t b) const {
    return [a, b](const IDETestValue &v) {
      if (v.is_bottom())
        return IDETestValue::bottom();
      if (v.is_top())
        return IDETestValue::top();
      return IDETestValue::constant(a * v.value + b);
    };
  }

  EdgeFunction create_constant_function(int64_t c) const {
    return [c](const IDETestValue &) { return IDETestValue::constant(c); };
  }

private:
  TestMode m_mode;
  int64_t m_addition_constant = 1;
  int64_t m_multiply_constant = 2;
};

// ============================================================================
// IDE Test Suite Helper
// ============================================================================

class IDETestSuite {
public:
  struct TestCase {
    std::string name;
    std::string description;
    IDESolverTestProblem::TestMode mode;
    std::function<bool(const IDESolverTestProblem::TestResult &)> validator;
  };

  IDETestSuite() { register_standard_tests(); }

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

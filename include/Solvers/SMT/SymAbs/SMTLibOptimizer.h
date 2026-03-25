#pragma once

#include "z3++.h"

#include <map>
#include <string>
#include <vector>

class SMTLibOptimizer {
public:
  enum OptimizationResult { OPT_SAT, OPT_UNSAT, OPT_UNKNOWN, OPT_ERROR };

  struct VariableInfo {
    enum SortKind { SORT_UNKNOWN, SORT_INT, SORT_BV };

    std::string name;
    std::string sort; // Store sort as string instead of z3::expr
    SortKind sort_kind;
    unsigned bv_width;
    long long min_value;
    long long max_value;
    bool found;
  };

private:
  z3::context &ctx;
  std::vector<VariableInfo> variables;
  std::string smt_formula;
  bool verbose;
  bool full_model;

  // Parse SMT-LIB formula and extract variable information
  bool parseSMTLibFormula();

  // Find a variable by name in the parsed formula
  bool findVariable(const std::string &var_name, VariableInfo &var_info);

  // Binary search to find maximum value of a variable
  long long binarySearchMax(const VariableInfo &var_info,
                            long long max_bound = 0,
                            bool *saw_unknown = nullptr);

  // Check satisfiability with a constraint on variable value
  z3::check_result checkWithConstraint(const VariableInfo &var_info,
                                       long long value);

  // Print solution information
  void printSolution(const VariableInfo &var_info, long long solution,
                     OptimizationResult result);

public:
  SMTLibOptimizer(z3::context &context, const std::string &smt_formula,
                  bool verbose = false, bool full_model = false);

  virtual ~SMTLibOptimizer() = default;

  // Set SMT-LIB formula
  void setFormula(const std::string &formula);

  // Get the SMT-LIB formula
  const std::string &getFormula() const { return smt_formula; }

  // Optimize a specific variable (find maximum value)
  OptimizationResult optimizeVariable(const std::string &var_name,
                                      long long &result);

  // Optimize multiple cut variables from a file
  bool optimizeCuts(const std::string &cuts_file);

  // Get all variables found in the formula
  const std::vector<VariableInfo> &getVariables() const { return variables; }

  // Set verbose output
  void setVerbose(bool v) { verbose = v; }

  // Set full model output
  void setFullModel(bool fm) { full_model = fm; }

  // Check if formula is valid SMT-LIB
  bool isValidFormula();
};

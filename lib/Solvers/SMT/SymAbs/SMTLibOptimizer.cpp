/**
 * @file SMTLibOptimizer.cpp
 * @brief Implementation of SMT-LIB formula optimizer
 *
 * This module provides functionality for optimizing variables in SMT-LIB format
 * formulas. It parses SMT-LIB strings, identifies variables, and uses binary
 * search to find optimal (minimum/maximum) values for variables subject to
 * formula constraints.
 *
 * **Key Features:**
 * - Parses SMT-LIB format formulas
 * - Identifies variable declarations
 * - Uses binary search to optimize variable values
 * - Supports batch optimization from files
 * - Configurable verbosity and error handling
 *
 * **Optimization Method:**
 * The optimizer uses binary search over the variable's value space to find the
 * maximum value. It iteratively tests constraints of the form "variable >=
 * value" to narrow down the search space.
 *
 * **Use Cases:**
 * - Optimizing variables in SMT-LIB formulas
 * - Batch optimization from specification files
 * - Finding bounds for variables in constraint systems
 *
 * **Limitations:**
 * - Currently supports integer variables (extended from SMT-LIB format)
 * - Binary search assumes non-negative values by default
 * - Optimization is limited to single-variable queries
 */

#include "Solvers/SMT/SymAbs/SMTLibOptimizer.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>

SMTLibOptimizer::SMTLibOptimizer(z3::context &context,
                                 const std::string &smt_formula, bool verbose,
                                 bool full_model)
    : ctx(context), smt_formula(smt_formula), verbose(verbose),
      full_model(full_model) {}

void SMTLibOptimizer::setFormula(const std::string &formula) {
  smt_formula = formula;
}

bool SMTLibOptimizer::findVariable(const std::string &var_name,
                                   VariableInfo &var_info) {
  std::istringstream iss(smt_formula);
  std::string line;

  while (std::getline(iss, line)) {
    if (line.find("declare-fun") == std::string::npos)
      continue;

    std::istringstream line_iss(line);
    std::vector<std::string> tokens;
    std::string token;

    while (line_iss >> token)
      tokens.push_back(token);

    if (tokens.size() >= 4 && tokens[0] == "(declare-fun" &&
        tokens[1] == var_name) {
      var_info.name = var_name;
      var_info.found = true;
      var_info.sort_kind = VariableInfo::SORT_UNKNOWN;
      var_info.bv_width = 0;

      if (tokens.size() > 3) {
        std::string sort_str = tokens.back();
        if (sort_str.back() == ')')
          sort_str.pop_back();
        var_info.sort = sort_str;
      }
      if (line.find(" Int") != std::string::npos ||
          line.find("Int)") != std::string::npos) {
        var_info.sort_kind = VariableInfo::SORT_INT;
        var_info.sort = "Int";
      } else if (line.find("BitVec") != std::string::npos) {
        std::regex bv_pattern(R"(\(_\s+BitVec\s+([0-9]+)\))");
        std::smatch match;
        if (std::regex_search(line, match, bv_pattern) && match.size() >= 2) {
          try {
            unsigned width = static_cast<unsigned>(std::stoul(match[1].str()));
            if (width > 0) {
              var_info.sort_kind = VariableInfo::SORT_BV;
              var_info.bv_width = width;
              var_info.sort = "(_ BitVec " + std::to_string(width) + ")";
            }
          } catch (...) {
            var_info.sort_kind = VariableInfo::SORT_UNKNOWN;
          }
        }
      }

      var_info.min_value = LLONG_MIN;
      var_info.max_value = LLONG_MAX;
      return true;
    }
  }
  return false;
}

z3::check_result
SMTLibOptimizer::checkWithConstraint(const VariableInfo &var_info,
                                     long long value) {
  z3::solver solver(ctx);

  try {
    z3::expr_vector formulas = ctx.parse_string(smt_formula.c_str());
    z3::expr var_expr(ctx);
    z3::expr constraint(ctx);

    if (var_info.sort_kind == VariableInfo::SORT_INT) {
      var_expr = ctx.int_const(var_info.name.c_str());
      constraint = var_expr >= ctx.int_val(static_cast<int64_t>(value));
    } else if (var_info.sort_kind == VariableInfo::SORT_BV) {
      var_expr = ctx.bv_const(var_info.name.c_str(), var_info.bv_width);
      uint64_t threshold = 0;
      if (value > 0) {
        threshold = static_cast<uint64_t>(value);
      }
      if (var_info.bv_width < 64) {
        const uint64_t max_u = (1ULL << var_info.bv_width) - 1ULL;
        threshold = std::min(threshold, max_u);
      }
      constraint = uge(var_expr, ctx.bv_val(threshold, var_info.bv_width));
    } else {
      return z3::check_result::unknown;
    }

    for (unsigned i = 0; i < formulas.size(); ++i)
      solver.add(formulas[i]);
    solver.add(constraint);

    if (verbose)
      std::cout << "Checking: " << var_info.name << " >= " << value << '\n';
    return solver.check();
  } catch (const z3::exception &e) {
    std::cerr << "Error: " << e.what() << '\n';
    return z3::check_result::unknown;
  }
}

long long SMTLibOptimizer::binarySearchMax(const VariableInfo &var_info,
                                           long long max_bound,
                                           bool *saw_unknown) {
  long long min_val = 0, max_val = max_bound > 0 ? max_bound : 1000000;
  long long best = -1;
  if (saw_unknown)
    *saw_unknown = false;

  while (min_val <= max_val) {
    long long mid = min_val + (max_val - min_val) / 2;
    z3::check_result result = checkWithConstraint(var_info, mid);

    if (result == z3::check_result::sat) {
      best = mid;
      min_val = mid + 1;
      if (verbose)
        std::cout << "SAT: " << var_info.name << " >= " << mid << '\n';
    } else if (result == z3::check_result::unsat) {
      max_val = mid - 1;
      if (verbose)
        std::cout << (result == z3::check_result::unsat ? "UNSAT" : "UNKNOWN")
                  << ": " << var_info.name << " >= " << mid << '\n';
    } else {
      if (saw_unknown)
        *saw_unknown = true;
      if (verbose)
        std::cout << "UNKNOWN: " << var_info.name << " >= " << mid << '\n';
      break;
    }
  }
  return best;
}

void SMTLibOptimizer::printSolution(const VariableInfo &var_info,
                                    long long solution,
                                    OptimizationResult result) {
  std::string msg = (result == OPT_SAT)       ? "The maximum value of "
                    : (result == OPT_UNKNOWN) ? "The probable maximum value of "
                                              : "No solution found for ";
  std::cout << msg << var_info.name << " is " << solution << "." << '\n';
}

SMTLibOptimizer::OptimizationResult
SMTLibOptimizer::optimizeVariable(const std::string &var_name,
                                  long long &result) {
  VariableInfo var_info = {};
  if (!findVariable(var_name, var_info)) {
    std::cerr << "Error: Variable " << var_name << " not found" << '\n';
    return OPT_ERROR;
  }
  if (var_info.sort_kind == VariableInfo::SORT_UNKNOWN) {
    std::cerr << "Error: Variable " << var_name
              << " has unsupported/unknown sort" << '\n';
    return OPT_ERROR;
  }

  if (verbose)
    std::cout << "Optimizing: " << var_name << '\n';

  bool saw_unknown = false;
  long long max_value = binarySearchMax(var_info, 0, &saw_unknown);
  result = max_value;
  if (max_value < 0) {
    return saw_unknown ? OPT_UNKNOWN : OPT_UNSAT;
  }

  z3::check_result check_result = checkWithConstraint(var_info, max_value);
  OptimizationResult opt_result = OPT_UNSAT;
  if (check_result == z3::check_result::sat) {
    opt_result = saw_unknown ? OPT_UNKNOWN : OPT_SAT;
  } else if (check_result == z3::check_result::unknown) {
    opt_result = OPT_UNKNOWN;
  }

  printSolution(var_info, max_value, opt_result);
  return opt_result;
}

bool SMTLibOptimizer::optimizeCuts(const std::string &cuts_file) {
  std::ifstream file(cuts_file.c_str());
  if (!file) {
    std::cerr << "Error: Cannot open cuts file: " << cuts_file << '\n';
    return false;
  }

  std::string line;
  int processed = 0, total = 0;

  while (std::getline(file, line)) {
    std::istringstream iss(line);
    std::string cut_name;
    int max_bound;

    if (!(iss >> cut_name >> max_bound))
      continue;

    total++;
    if (verbose)
      std::cout << "Processing cut: " << cut_name << '\n';

    VariableInfo var_info = {};
    if (findVariable(cut_name, var_info)) {
      if (var_info.sort_kind == VariableInfo::SORT_UNKNOWN) {
        if (verbose) {
          std::cout << "Warning: Cut variable " << cut_name
                    << " has unsupported/unknown sort" << '\n';
        }
        continue;
      }
      processed++;
      if (verbose)
        std::cout << "Cut " << processed << "/" << total << ": " << cut_name
                  << '\n';

      long long solution = binarySearchMax(var_info, max_bound, nullptr);
      if (verbose)
        std::cout << "Cost: " << solution << '\n';
    } else if (verbose) {
      std::cout << "Warning: Cut variable " << cut_name << " not found" << '\n';
    }
  }

  if (verbose)
    std::cout << "Processed " << processed << "/" << total << " cuts" << '\n';
  return processed > 0;
}

bool SMTLibOptimizer::isValidFormula() {
  if (smt_formula.empty())
    return false;

  try {
    ctx.parse_string(smt_formula.c_str());
    return true;
  } catch (const z3::exception &) {
    return false;
  }
}

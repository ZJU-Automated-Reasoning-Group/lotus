/**
 * @file IntervalSampler.cpp
 * @brief Implementation of interval_sampler - a bounds-based approach for
 * sampling SMT formulas
 *
 * Fixes applied (original B-series):
 *  B10 – get_bounds() now uses lower() for the minimization result and upper()
 *        for the maximization result (they were swapped).
 *  B11 – Bounds are stored as int64_t (not int) and extracted via
 *        get_numeral_int64() to avoid silent truncation for wide bit-vectors.
 *  B12 – The fallback upper bound uses (1ULL << sz) - 1 (uint64_t arithmetic)
 *        instead of (1 << sz) - 1, which is UB for sz >= 32.
 *  B13 – check_random_model() uses an unordered_set<string> for O(1) amortized
 *        uniqueness checking instead of O(n) linear scan.
 *  B14 – m_unique is reset in reset_state() so it stays in sync with
 *        unique_models across files.
 *  B15 – Directory traversal filters entries to only process files whose names
 *        end in ".smt2".
 *  B16 – closedir() is now called on all exit paths from the directory branch.
 *  B17 – The timeout clock (init) is reset just before the sampling loop so
 *        that bound-computation time does not consume the sampling budget.
 *  B18 – m_sample_time now accumulates per-iteration durations (finish -
 * iter_start) rather than cumulative elapsed time from the loop start.
 *
 * Additional fixes (new):
 *  IS-2 – get_bounds() wraps get_numeral_int64() calls in try/catch so that
 *          unbounded variables (whose bound expression is ±∞) do not crash the
 *          process; the fallback default is used instead.
 *  IS-3 – sample_once() computes the range via unsigned arithmetic to avoid
 *          signed int64_t overflow when lo is negative and hi is INT64_MAX.
 */

#include "Solvers/SMT/SMTSampler/SMTSampler.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

using namespace std;
using namespace z3;

namespace {
constexpr const char *kSamplerName = "IntervalSampler";

void log_info(const std::string &msg) {
  std::cout << "[" << kSamplerName << "] " << msg << '\n';
}

void log_warn(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] WARN: " << msg << '\n';
}

void log_error(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] ERROR: " << msg << '\n';
}

std::string join_path(const std::string &dir, const std::string &name) {
  if (dir.empty())
    return name;
  if (dir.back() == '/')
    return dir + name;
  return dir + "/" + name;
}

/// Returns true iff the string ends with the given suffix.
bool ends_with(const std::string &s, const std::string &suffix) {
  if (suffix.size() > s.size())
    return false;
  return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace

struct interval_sampler {
  std::string path;
  std::string input_file;
  std::vector<std::string> input_files;

  struct timespec start_time;
  double solver_time = 0.0;
  double check_time = 0.0;
  int max_samples;
  double max_time;

  int m_samples = 0;
  int m_success = 0;
  int m_unique = 0;
  double m_sample_time = 0.0;
  bool stop_requested = false;
  std::string stop_reason;

  z3::context c;
  z3::expr smt_formula;
  z3::expr_vector m_vars;

  // B11: use int64_t for bounds to avoid truncation of wide bit-vectors.
  std::vector<int64_t> lower_bounds;
  std::vector<int64_t> upper_bounds;
  std::vector<bool> should_fix;

  // B13: use a hash set for O(1) uniqueness checking.
  std::unordered_set<std::string> unique_model_set;

  std::mt19937_64 rng;

  interval_sampler(std::string &input, int max_samples, double max_time)
      : path(input), max_samples(max_samples), max_time(max_time),
        smt_formula(c), m_vars(c) {
    auto seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    rng.seed(seed);

    struct stat info = {};
    if (stat(path.c_str(), &info) == 0) {
      if (info.st_mode & S_IFDIR) {
        // B15: only collect .smt2 files; B16: always call closedir.
        DIR *dirp = opendir(input.c_str());
        if (dirp) {
          struct dirent *dp;
          while ((dp = readdir(dirp)) != nullptr) {
            std::string tmp(dp->d_name);
            if (tmp == "." || tmp == "..")
              continue;
            // B15: skip non-.smt2 files.
            if (!ends_with(tmp, ".smt2"))
              continue;
            input_files.push_back(join_path(path, tmp));
          }
          closedir(dirp); // B16: always reached now.
        } else {
          log_warn("Could not open directory: " + input);
        }
      } else {
        input_files.push_back(input);
      }
    } else {
      input_files.push_back(input);
    }
    input_file = input;
  }

  void parse_smt() {
    try {
      expr_vector evec = c.parse_file(input_file.c_str());
      smt_formula = mk_and(evec);
    } catch (const z3::exception &e) {
      log_error(std::string("Failed to parse SMT file: ") + e.msg());
      smt_formula = z3::expr(c);
    }
  }

  /**
   * @brief Computes per-variable bounds using Z3 optimizer.
   *
   * B10: lower() gives the optimal (minimum) value after minimize();
   *      upper() gives the optimal (maximum) value after maximize().
   *      The original code had these swapped.
   * B11: bounds stored as int64_t; extracted via get_numeral_int64().
   * B12: fallback upper bound uses uint64_t shift to avoid UB.
   * IS-2: get_numeral_int64() calls are wrapped in try/catch so that
   *        unbounded variables (±∞ expressions) do not crash the process.
   */
  void get_bounds() {
    params p(c);
    p.set("priority", c.str_symbol("box"));
    p.set("timeout", (unsigned)15000);

    optimize opt_sol_min(c);
    opt_sol_min.set(p);
    opt_sol_min.add(smt_formula);

    optimize opt_sol_max(c);
    opt_sol_max.set(p);
    opt_sol_max.add(smt_formula);

    // Minimize each variable.
    std::vector<optimize::handle> handlers_min;
    for (unsigned i = 0; i < m_vars.size(); i++)
      handlers_min.push_back(opt_sol_min.minimize(m_vars[i]));
    auto min_res = opt_sol_min.check();
    if (min_res != sat)
      log_warn("Minimize check not sat; using defaults");

    for (unsigned i = 0; i < m_vars.size(); i++) {
      if (min_res == sat) {
        // B10: use lower() (optimal value) not upper() (bound on optimum).
        // IS-2: catch exceptions from get_numeral_int64() on ±∞ expressions.
        try {
          lower_bounds.push_back(
              opt_sol_min.lower(handlers_min[i]).get_numeral_int64());
        } catch (const z3::exception &e) {
          log_warn(std::string("lower bound extraction failed (unbounded?): ") +
                   e.msg() + "; defaulting to 0");
          lower_bounds.push_back(0);
        }
      } else {
        lower_bounds.push_back(0);
      }
    }

    // Maximize each variable.
    std::vector<optimize::handle> handlers_max;
    for (unsigned i = 0; i < m_vars.size(); i++)
      handlers_max.push_back(opt_sol_max.maximize(m_vars[i]));
    auto max_res = opt_sol_max.check();
    if (max_res != sat)
      log_warn("Maximize check not sat; using defaults");

    for (unsigned i = 0; i < m_vars.size(); i++) {
      if (max_res == sat) {
        // B10: use upper() (optimal value) not lower() (bound on optimum).
        // IS-2: catch exceptions from get_numeral_int64() on ±∞ expressions.
        try {
          upper_bounds.push_back(
              opt_sol_max.upper(handlers_max[i]).get_numeral_int64());
        } catch (const z3::exception &e) {
          log_warn(std::string("upper bound extraction failed (unbounded?): ") +
                   e.msg() + "; using bit-width default");
          unsigned sz = m_vars[i].get_sort().bv_size();
          // B12: use uint64_t shift to avoid UB for sz >= 32.
          int64_t max_val;
          if (sz >= 64)
            max_val = std::numeric_limits<int64_t>::max();
          else
            max_val = static_cast<int64_t>((1ULL << sz) - 1ULL);
          upper_bounds.push_back(max_val);
        }
      } else {
        unsigned sz = m_vars[i].get_sort().bv_size();
        // B12: use uint64_t shift to avoid UB for sz >= 32.
        int64_t max_val;
        if (sz >= 64)
          max_val = std::numeric_limits<int64_t>::max();
        else
          max_val = static_cast<int64_t>((1ULL << sz) - 1ULL);
        upper_bounds.push_back(max_val);
      }
    }
  }

  /**
   * @brief Generates a single sample by choosing values uniformly within
   * bounds.
   *
   * B11: uses int64_t arithmetic throughout.
   * IS-3: range is computed via unsigned arithmetic to avoid signed int64_t
   *        overflow when lo is negative and hi is close to INT64_MAX.
   */
  std::vector<int64_t> sample_once() {
    m_samples++;
    std::vector<int64_t> sample;
    sample.reserve(m_vars.size());
    for (unsigned i = 0; i < m_vars.size(); i++) {
      if (should_fix[i]) {
        sample.push_back(lower_bounds[i]);
        continue;
      }
      int64_t lo = lower_bounds[i];
      int64_t hi = upper_bounds[i];
      if (lo >= hi) {
        sample.push_back(lo);
        continue;
      }
      // IS-3: cast to uint64_t before subtraction to avoid signed overflow
      // when lo is negative and hi is large (e.g., lo = INT64_MIN, hi =
      // INT64_MAX).
      uint64_t range =
          static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1ULL;
      // If range wrapped to 0 (lo == INT64_MIN, hi == INT64_MAX), use max
      // range.
      if (range == 0)
        range = std::numeric_limits<uint64_t>::max();
      std::uniform_int_distribution<uint64_t> dist(0, range - 1);
      int64_t output = lo + static_cast<int64_t>(dist(rng));
      sample.push_back(output);
    }
    return sample;
  }

  /**
   * @brief Serialises a sample to a string for uniqueness checking.
   */
  static std::string sample_key(const std::vector<int64_t> &s) {
    std::ostringstream oss;
    for (size_t i = 0; i < s.size(); ++i) {
      if (i)
        oss << ',';
      oss << s[i];
    }
    return oss.str();
  }

  /**
   * @brief Verifies if the generated sample satisfies the original SMT formula.
   *
   * B11: assignments are int64_t.
   * B13: uniqueness check is O(1) via unordered_set.
   */
  bool check_random_model(std::vector<int64_t> &assignments) {
    model rand_model(c);
    for (unsigned i = 0; i < m_vars.size(); i++) {
      z3::func_decl decl = m_vars[i].decl();
      unsigned sz = m_vars[i].get_sort().bv_size();
      // Use uint64_t to correctly represent the bit pattern.
      uint64_t uval = static_cast<uint64_t>(assignments[i]);
      z3::expr val_i = c.bv_val(uval, sz);
      rand_model.add_const_interp(decl, val_i);
    }

    if (!rand_model.eval(smt_formula, true).is_true())
      return false;

    // B13: O(1) uniqueness check.
    std::string key = sample_key(assignments);
    if (unique_model_set.insert(key).second) {
      m_unique++;
    }
    return true;
  }

  /**
   * @brief Main execution function.
   *
   * B17: timeout clock reset just before the sampling loop.
   * B18: m_sample_time accumulates per-iteration durations.
   */
  void run() {
    for (auto &file : input_files) {
      reset_state();
      lower_bounds.clear();
      upper_bounds.clear();
      should_fix.clear();
      stop_requested = false;
      stop_reason.clear();
      input_file = file;
      parse_smt();
      if (!smt_formula) {
        log_error("Skipping file with parse failure: " + input_file);
        continue;
      }
      log_info("Parsed SMT input: " + input_file);

      m_vars = z3::expr_vector(c);
      get_expr_vars(smt_formula, m_vars);
      log_info("Collected variables; computing bounds");

      auto bound_start = std::chrono::high_resolution_clock::now();
      get_bounds();
      auto bound_end = std::chrono::high_resolution_clock::now();
      solver_time +=
          std::chrono::duration<double, std::milli>(bound_end - bound_start)
              .count();

      for (unsigned i = 0; i < m_vars.size(); i++) {
        should_fix.push_back(lower_bounds[i] == upper_bounds[i]);
      }
      log_info("Bounds computed; sampling models");

      // B17: reset the sampling clock *after* bound computation so the
      // full max_time budget is available for actual sampling.
      auto init = std::chrono::high_resolution_clock::now();

      for (int i = 0; i < max_samples; i++) {
        if (i % 5000 == 0)
          print_stats();
        if (stop_requested)
          break;

        auto iter_start = std::chrono::high_resolution_clock::now();
        double elapsed =
            std::chrono::duration<double, std::milli>(iter_start - init)
                .count();
        if (elapsed >= max_time) {
          log_warn("Stopping: timeout");
          request_stop("timeout");
          break;
        }

        std::vector<int64_t> sample = sample_once();
        if (check_random_model(sample))
          m_success++;

        // B18: accumulate per-iteration time, not cumulative elapsed time.
        auto iter_end = std::chrono::high_resolution_clock::now();
        m_sample_time +=
            std::chrono::duration<double, std::milli>(iter_end - iter_start)
                .count();
      }
      if (stop_requested)
        log_info("Stopped due to " + stop_reason);
      print_stats();
    }
  }

  void request_stop(const std::string &reason) {
    stop_requested = true;
    stop_reason = reason;
  }

  void reset_state() {
    solver_time = 0;
    m_sample_time = 0;
    m_samples = 0;
    m_success = 0;
    m_unique = 0; // B14: reset m_unique alongside unique_model_set.
    unique_model_set.clear();
  }

  void print_stats() {
    std::cout << "solver time: " << solver_time << "\n";
    std::cout << "sample total time: " << m_sample_time << "\n";
    std::cout << "samples number: " << m_samples << "\n";
    std::cout << "samples success: " << m_success << "\n";
    std::cout << "unique models: " << m_unique << "\n";
    std::cout << "------------------------------------------\n";

    // Append stats to res.log, prefixed with the current input file name so
    // that results from different files in a directory run are distinguishable.
    std::ofstream of("res.log", std::ofstream::app);
    if (!of.is_open()) {
      log_warn("Failed to open res.log for append");
      return;
    }
    of << "file: " << input_file << "\n";
    of << "solver time: " << solver_time << "\n";
    of << "sample total time: " << m_sample_time << "\n";
    of << "samples number: " << m_samples << "\n";
    of << "samples success: " << m_success << "\n";
    of << "unique models: " << m_unique << "\n";
    of << "------------------------------------------\n";
    of.close();
  }
};

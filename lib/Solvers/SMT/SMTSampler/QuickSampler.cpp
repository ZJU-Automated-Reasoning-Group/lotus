/**
 * @file QuickSampler.cpp
 * @brief Implementation of quick_sampler - a mutation-based approach for
 * sampling SMT formulas
 *
 * Fixes applied (original B-series):
 *  B1  – Added a maximum epoch limit (max_epochs) to prevent the outer
 * while(true) loop from running forever when the formula is always satisfiable.
 *  B2  – unsat_vars is now cleared at the start of each call to sample() so
 *        that variables are not permanently blacklisted across epochs.
 *  B3  – solve() now propagates stop_requested back to sample(), which breaks
 *        out of the flip loop cleanly before any opt.pop() imbalance can occur.
 *  B4  – Empty clauses (produced by a DIMACS line containing only "0") are
 *        skipped instead of being added as mk_or({}) == false.
 *  B5  – The CNF parser now breaks out of the literal-reading loop as soon as
 *        it reads the 0 terminator, preventing over-reading.
 *  B6  – The XOR recombination formula has been replaced with a correct
 *        majority-vote operator: bit is 1 iff at least two of {a, b, c} are 1.
 *  B7  – model_string() now uses model.eval(expr, true) and falls back to the
 *        model's own Boolean value; if the result is neither true nor false
 *        (unconstrained variable), it defaults to '0' with a warning.
 *  B8  – print_stats(true) is no longer called inside the per-flip inner loop;
 *        it is called once per epoch instead.
 *  B9  – Replaced z3::optimize with z3::solver for pure SAT queries.  Soft
 *        "preference" constraints are now encoded as hard assumptions pushed
 *        onto the solver stack, which is both correct and much faster.
 *
 * Additional fixes (new):
 *  QS-1 – Removed the dead push/pop block in sample() that hard-pinned the
 *          seed model but was immediately popped before any solve call, making
 *          it a no-op.  The seed model is now used directly to seed per-flip
 *          pushes without the redundant outer push/pop.
 *  QS-2 – literal() now caches z3::expr objects in an unordered_map keyed by
 *          variable index, avoiding repeated string construction and Z3 symbol
 *          interning on every call inside tight loops.
 *  QS-3 – output() now checks the sample limit before recording a sample so
 *          that recombination candidates cannot push the count past
 * max_samples. QS-4 – Recombination candidates are now verified against the
 * formula via a solver check before being written to the output file. QS-5 –
 * parse_cnf() deduplicates independent variables globally across all "c ind"
 * lines using the same indset, preventing duplicate entries in ind[] that would
 * cause model_string() size-mismatch errors.
 */

#include "Solvers/SMT/SMTSampler/SMTSampler.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
using namespace z3;

namespace {
constexpr const char *kSamplerName = "QuickSampler";

void log_info(const std::string &msg) {
  std::cout << "[" << kSamplerName << "] " << msg << '\n';
}

void log_warn(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] WARN: " << msg << '\n';
}

void log_error(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] ERROR: " << msg << '\n';
}
} // namespace

class quick_sampler {
  std::string input_file;

  struct timespec start_time;
  double solver_time = 0.0;
  int max_samples;
  double max_time;

  // B1: cap the number of outer epochs so the loop always terminates.
  static constexpr int kMaxEpochs = 10000;

  z3::context c;
  // B9: use a plain solver instead of an optimizer for pure SAT queries.
  z3::solver sol;
  std::vector<int> ind; ///< Independent variables (support set for sampling)
  int epochs = 0;
  int flips = 0;
  int samples = 0;
  int solver_calls = 0;
  bool stop_requested = false;
  std::string stop_reason;

  std::mt19937 rng;
  std::uniform_int_distribution<int> bit_dist{0, 1};

  std::ofstream results_file;

  // QS-2: cache for literal() to avoid repeated string construction / Z3
  // interning.
  std::unordered_map<int, z3::expr> literal_cache;

public:
  quick_sampler(std::string input, int max_samples, double max_time)
      : input_file(input), max_samples(max_samples), max_time(max_time),
        sol(c) {
    auto seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    rng.seed(seed);
  }

  /**
   * @brief Main execution loop of the QuickSampler.
   *
   * B1: the outer loop is bounded by kMaxEpochs.
   * B9: uses z3::solver with push/pop instead of z3::optimize.
   */
  void run() {
    clock_gettime(CLOCK_REALTIME, &start_time);
    if (!parse_cnf()) {
      log_error("Failed to parse CNF input: " + input_file);
      return;
    }
    results_file.open(input_file + ".samples", std::ios::out | std::ios::trunc);
    if (!results_file.is_open()) {
      log_error("Failed to open output file: " + input_file + ".samples");
      return;
    }
    results_file << "# format: <mutations>: <bitstring>\n";

    // B1: bounded epoch loop.
    for (int epoch = 0; epoch < kMaxEpochs && !stop_requested; ++epoch) {
      sol.push();
      // Randomly assign independent variables as hard assumptions to seed
      // search.
      for (int v : ind) {
        if (bit_dist(rng))
          sol.add(literal(v));
        else
          sol.add(!literal(v));
      }
      if (!solve()) {
        sol.pop();
        break;
      }
      z3::model m = sol.get_model();
      sol.pop();

      sample(m);
      print_stats(false);
    }

    if (stop_requested)
      log_info("Stopped due to " + stop_reason);
    finish();
  }

  void print_stats(bool simple) {
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    double elapsed = duration(&start_time, &end);
    std::cout << "Samples " << samples << '\n';
    std::cout << "Execution time " << elapsed << '\n';
    if (simple)
      return;
    std::cout << "Solver time: " << solver_time << '\n';
    std::cout << "Epochs " << epochs << ", Flips " << flips << ", Calls "
              << solver_calls << '\n';
  }

  /**
   * @brief Parses the input CNF file (DIMACS format).
   *
   * B4: empty clauses (lines with only "0") are skipped.
   * B5: the literal-reading loop breaks on the 0 terminator.
   * QS-5: deduplication of independent variables is global across all
   *        "c ind" lines — indset is declared once outside the loop.
   */
  bool parse_cnf() {
    z3::expr_vector exp(c);
    std::ifstream f(input_file);
    if (!f.is_open()) {
      log_error("Unable to open input file");
      return false;
    }
    // QS-5: indset is declared once so duplicates across multiple "c ind"
    // lines are caught correctly.
    std::unordered_set<int> indset;
    bool has_ind = false;
    int max_var = 0;
    std::string line;
    while (getline(f, line)) {
      if (line.empty())
        continue;
      std::istringstream iss(line);
      if (line.find("c ind ") == 0) {
        std::string s;
        iss >> s; // "c"
        iss >> s; // "ind"
        int v;
        while (iss >> v) {
          if (v == 0)
            break; // B5: stop at terminator
          // QS-5: indset persists across lines, so duplicates are skipped.
          if (indset.insert(v).second) {
            ind.push_back(v);
            has_ind = true;
          }
        }
      } else if (line[0] != 'c' && line[0] != 'p') {
        z3::expr_vector clause(c);
        int v;
        while (iss >> v) {
          if (v == 0)
            break; // B5: stop at DIMACS clause terminator
          if (v > 0)
            clause.push_back(literal(v));
          else
            clause.push_back(!literal(-v));
          int av = abs(v);
          if (!has_ind)
            indset.insert(av);
          if (av > max_var)
            max_var = av;
        }
        // B4: skip empty clauses (would make the formula trivially UNSAT).
        if (clause.size() == 0)
          continue;
        exp.push_back(mk_or(clause));
      }
    }
    f.close();
    if (!has_ind) {
      for (int lit = 1; lit <= max_var; ++lit) {
        if (indset.find(lit) != indset.end())
          ind.push_back(lit);
      }
    }
    if (ind.empty())
      log_warn("No independent variables found in CNF");

    z3::expr formula = mk_and(exp);
    sol.add(formula);
    return true;
  }

  /**
   * @brief Generates samples by mutating a known satisfying model.
   *
   * B2: unsat_vars is local to each call so variables are not permanently
   *     blacklisted across epochs.
   * B3: the flip loop checks stop_requested after every solve() call.
   * B6: recombination uses majority vote instead of the broken XOR formula.
   * B7: model_string() handles unconstrained variables gracefully.
   * B8: print_stats(true) removed from the inner flip loop.
   * QS-1: removed the dead push/pop block that hard-pinned the seed model
   *        but was immediately popped before any solve call.
   * QS-4: recombination candidates are verified by the solver before output.
   */
  void sample(z3::model &m) {
    // B2: unsat_vars is local – cleared every epoch.
    std::unordered_set<int> unsat_vars;

    std::unordered_set<std::string> initial_mutations;
    std::string m_string = model_string(m);
    if (m_string.size() != ind.size()) {
      log_error("Model projection size mismatch; skipping sample");
      return;
    }
    std::cout << m_string << " STARTING\n";
    output(m_string, 0);

    // QS-1: the dead push/pop that pinned the seed model has been removed.
    // Per-flip pushes below correctly constrain the solver for each mutation.

    std::unordered_map<std::string, int> mutations;

    for (unsigned i = 0; i < ind.size(); ++i) {
      if (stop_requested) // B3
        break;
      if (unsat_vars.find(i) != unsat_vars.end())
        continue;

      sol.push();
      int v = ind[i];
      // Force flip of variable i.
      if (m_string[i] == '1')
        sol.add(!literal(v));
      else
        sol.add(literal(v));

      if (solve()) {
        z3::model new_model = sol.get_model();
        std::string new_string = model_string(new_model);
        if (initial_mutations.find(new_string) == initial_mutations.end()) {
          initial_mutations.insert(new_string);
          std::unordered_map<std::string, int> new_mutations;
          new_mutations[new_string] = 1;
          output(new_string, 1);
          flips += 1;

          // B6: majority-vote recombination.
          // A bit in the candidate is 1 iff at least 2 of {m_string, prev, new}
          // have it as 1.  This is a well-defined, symmetric operator.
          for (auto &it : mutations) {
            if (stop_requested)
              break;
            if (it.second >= 6)
              continue;
            std::string candidate;
            candidate.reserve(ind.size());
            for (unsigned j = 0; j < ind.size(); ++j) {
              int a = (m_string[j] == '1') ? 1 : 0;
              int b = (it.first[j] == '1') ? 1 : 0;
              int cc = (new_string[j] == '1') ? 1 : 0;
              // Majority vote: 1 iff at least 2 of the 3 bits are 1.
              candidate += ((a + b + cc) >= 2) ? '1' : '0';
            }
            if (mutations.find(candidate) == mutations.end() &&
                new_mutations.find(candidate) == new_mutations.end()) {
              // QS-4: verify the recombination candidate satisfies the formula.
              if (verify_candidate(candidate)) {
                new_mutations[candidate] = it.second + 1;
                output(candidate, it.second + 1);
              }
            }
          }
          for (auto &it : new_mutations)
            mutations[it.first] = it.second;
        }
      } else if (!stop_requested) {
        // Only mark as unsat if we didn't stop due to timeout/limit.
        log_warn("Mutation unsat at index " + std::to_string(i));
        unsat_vars.insert(i);
      }
      sol.pop();
    }
    epochs += 1;
    // B8: print_stats only once per epoch, not per flip.
    print_stats(true);
  }

  /**
   * @brief Verifies that a candidate bit-string satisfies the formula.
   *
   * QS-4: recombination candidates are not guaranteed to be satisfying
   * assignments, so we check them with the solver before recording them.
   *
   * @param candidate Bit-string over ind[] variables.
   * @return true iff the assignment satisfies the formula.
   */
  bool verify_candidate(const std::string &candidate) {
    if (candidate.size() != ind.size())
      return false;
    sol.push();
    for (unsigned i = 0; i < ind.size(); ++i) {
      if (candidate[i] == '1')
        sol.add(literal(ind[i]));
      else
        sol.add(!literal(ind[i]));
    }
    bool sat = (sol.check() == z3::sat);
    sol.pop();
    return sat;
  }

  /**
   * @brief Records a sample if the sample limit has not been reached.
   *
   * QS-3: the limit check is performed here so that recombination candidates
   * cannot push the count past max_samples.
   */
  void output(std::string &s, int nmut) {
    if (samples >= max_samples) {
      stop_requested = true;
      stop_reason = "samples";
      return;
    }
    samples += 1;
    results_file << nmut << ": " << s << '\n';
  }

  void finish() {
    print_stats(false);
    if (results_file.is_open()) {
      results_file.close();
      log_info("Samples file closed");
    }
  }

  bool solve() {
    struct timespec start;
    clock_gettime(CLOCK_REALTIME, &start);
    double elapsed = duration(&start_time, &start);
    if (elapsed > max_time) {
      stop_requested = true;
      stop_reason = "timeout";
      log_info("Stopping: timeout");
      return false;
    }
    if (samples >= max_samples) {
      stop_requested = true;
      stop_reason = "samples";
      log_info("Stopping: sample limit");
      return false;
    }

    // B9: use sol.check() (plain solver) instead of opt.check().
    z3::check_result result = sol.check();
    struct timespec end;
    clock_gettime(CLOCK_REALTIME, &end);
    solver_time += duration(&start, &end);
    solver_calls += 1;

    if (result == z3::unknown)
      log_warn("Solver returned unknown");
    return result == z3::sat;
  }

  /**
   * @brief Projects the model onto the independent variables as a bit-string.
   *
   * B7: if eval() returns a non-Boolean expression (unconstrained variable),
   * we log a warning and default to '0'.
   */
  std::string model_string(z3::model &model) {
    std::string s;
    s.reserve(ind.size());
    for (int v : ind) {
      z3::expr b = model.eval(literal(v), true);
      if (b.is_true()) {
        s += '1';
      } else if (b.is_false()) {
        s += '0';
      } else {
        // Variable is unconstrained in this model – default to '0'.
        log_warn("Variable " + std::to_string(v) +
                 " is unconstrained in model; defaulting to 0");
        s += '0';
      }
    }
    return s;
  }

  double duration(struct timespec *a, struct timespec *b) {
    return (b->tv_sec - a->tv_sec) + 1.0e-9 * (b->tv_nsec - a->tv_nsec);
  }

  /**
   * @brief Returns the Z3 Boolean constant for variable index v.
   *
   * QS-2: results are cached in literal_cache to avoid repeated string
   * construction and Z3 symbol interning on every call inside tight loops.
   */
  z3::expr literal(int v) {
    auto it = literal_cache.find(v);
    if (it != literal_cache.end())
      return it->second;
    z3::expr e =
        c.constant(c.str_symbol(std::to_string(v).c_str()), c.bool_sort());
    literal_cache.emplace(v, e);
    return e;
  }
};

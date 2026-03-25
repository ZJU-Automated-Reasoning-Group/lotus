#pragma once

//===----------------------------------------------------------------------===//
/// @file Microbench.h
/// @brief Microbenchmarking utilities for performance measurement
///
/// This file provides classes and functions for measuring the execution time
/// of code snippets with high precision. It is based on the microbench library
/// by Cameron McKinnon (https://github.com/cameron314/microbench).
///
/// The utilities support:
/// - Configurable timing resolution (nanoseconds, microseconds, milliseconds)
/// - Multiple iterations and runs for statistical accuracy
/// - Automatic computation of statistical measures (min, max, avg, stddev)
/// - Percentile reporting (q1, median/q2, q3)
///
/// @par Usage Example:
/// @code
/// #include "Utils/Benchmark/Microbench.h"
///
/// void myFunction() {
///     // Function code to benchmark
/// }
///
/// auto stats = ccutils::microbenchStats<std::chrono::microseconds, 100,
/// 50>(myFunction); double avgTime = stats.avg(); double p95 = stats.q3();
/// @endcode
///
///===----------------------------------------------------------------------===//

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>

namespace ccutils {

/// @brief Statistics calculator for microbenchmark results
///
/// This class computes statistical measures from a collection of timing
/// measurements. It calculates min, max, average, variance, standard deviation,
/// and percentiles (q1, median/q2, q3).
///
/// @note The input vector is sorted in-place during computation.
class Stats {
public:
  /// @brief Construct a Stats object from timing results
  /// @param results Vector of timing measurements (will be sorted)
  Stats(std::vector<double> &results) {
    auto n = results.size();
    std::sort(results.begin(), results.end());

    _min = results[0];
    _max = results.back();

    if (n == 1) {
      _q[0] = _q[1] = _q[2] = results[0];
      _avg = results[0];
      _variance = 0;
      return;
    }

    double sum = 0;
    double c = 0;
    for (auto r : results) {
      auto y = r - c;
      auto t = sum + y;
      c = (t - sum) - y;
      sum = t;
    }
    _avg = sum / n;

    sum = 0, c = 0;
    for (auto r : results) {
      auto y = (r - _avg) * (r - _avg) - c;
      auto t = sum + y;
      c = (t - sum) - y;
      sum = t;
    }
    _variance = sum / (n - 1);

    _q[1] = (n & 1) == 0 ? (results[n / 2 - 1] + results[n / 2]) * 0.5
                         : results[n / 2];
    if ((n & 1) == 0) {
      _q[0] = (n & 3) == 0 ? (results[n / 4 - 1] + results[n / 4]) * 0.5
                           : results[n / 4];
      _q[2] = (n & 3) == 0
                  ? (results[n / 2 + n / 4 - 1] + results[n / 2 + n / 4]) * 0.5
                  : results[n / 2 + n / 4];
    } else if ((n & 3) == 1) {
      _q[0] = results[n / 4 - 1] * 0.25 + results[n / 4] * 0.75;
      _q[2] = results[n / 4 * 3] * 0.75 + results[n / 4 * 3 + 1] * 0.25;
    } else {
      _q[0] = results[n / 4] * 0.75 + results[n / 4 + 1] * 0.25;
      _q[2] = results[n / 4 * 3 + 1] * 0.25 + results[n / 4 * 3 + 2] * 0.75;
    }
  }

  /// @brief Get the minimum timing value
  /// @return The smallest measurement in the dataset
  inline double min() const { return _min; }
  /// @brief Get the maximum timing value
  /// @return The largest measurement in the dataset
  inline double max() const { return _max; }
  /// @brief Get the range (max - min)
  /// @return The difference between max and min values
  inline double range() const { return _max - _min; }
  /// @brief Get the average (arithmetic mean)
  /// @return The mean of all measurements
  inline double avg() const { return _avg; }
  /// @brief Get the variance
  /// @return The statistical variance of the measurements
  inline double variance() const { return _variance; }
  /// @brief Get the standard deviation
  /// @return The square root of variance
  inline double stddev() const { return std::sqrt(_variance); }
  /// @brief Get the median (50th percentile)
  /// @return The median value (q2)
  inline double median() const { return _q[1]; }
  /// @brief Get the first quartile (25th percentile)
  /// @return The q1 value
  inline double q1() const { return _q[0]; }
  /// @brief Get the second quartile (50th percentile, same as median)
  /// @return The q2 value
  inline double q2() const { return _q[1]; }
  /// @brief Get the third quartile (75th percentile)
  /// @return The q3 value
  inline double q3() const { return _q[2]; }

private:
  double _min;
  double _max;
  double _q[3];
  double _avg;
  double _variance;
};

/// @brief Run a microbenchmark and return statistics
///
/// This template function executes the provided function multiple times,
/// measures the execution time, and computes statistical measures.
///
/// @tparam Resolution The chrono duration type for timing (default:
/// nanoseconds)
/// @tparam iter Number of iterations per run (default: 1)
/// @tparam run Number of benchmark runs (default: 100)
/// @tparam timePerIter If true, divide total time by iterations (default: true)
/// @tparam TFunc The type of the function to benchmark
/// @param func The function to benchmark
/// @return A Stats object containing timing statistics
///
/// @par Example:
/// @code
/// auto stats = ccutils::microbenchStats<std::chrono::microseconds, 10, 50>([]{
///     // code to benchmark
/// });
/// std::cout << "Average: " << stats.avg() << " us\n";
/// @endcode
template <typename Resolution = std::chrono::nanoseconds, std::size_t iter = 1,
          std::size_t run = 100, bool timePerIter = true, typename TFunc>
Stats microbenchStats(TFunc &&func) {
  static_assert(run >= 1);
  static_assert(iter >= 1);

  std::vector<double> results(run);
  for (std::size_t i = 0; i < run; ++i) {
    auto start = std::chrono::steady_clock::now();
    std::atomic_signal_fence(std::memory_order_acq_rel);
    for (std::size_t j = 0; j < iter; ++j) {
      func();
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
    auto t = std::chrono::steady_clock::now();
    results[i] = std::chrono::duration_cast<Resolution>(t - start).count();
    if (timePerIter) {
      results[i] /= iter;
    }
  }

  double fastest = results[0];
  for (std::size_t i = 1; i < run; ++i) {
    if (results[i] < fastest) {
      fastest = results[i];
    }
  }

  Stats stats(results);
  return stats;
}

/// @brief Run a microbenchmark and return the average execution time
///
/// This is a convenience wrapper around microbenchStats that returns only
/// the average execution time.
///
/// @tparam Resolution The chrono duration type for timing (default:
/// nanoseconds)
/// @tparam iter Number of iterations per run (default: 1)
/// @tparam run Number of benchmark runs (default: 100)
/// @tparam timePerIter If true, divide total time by iterations (default: true)
/// @tparam TFunc The type of the function to benchmark
/// @param func The function to benchmark
/// @return The average execution time in the specified resolution
template <typename Resolution = std::chrono::nanoseconds, std::size_t iter = 1,
          std::size_t run = 100, bool timePerIter = true, typename TFunc>
inline __attribute__((always_inline)) double microbench(TFunc &&func) {
  return microbenchStats<Resolution, iter, run, timePerIter, TFunc>(
             std::forward<TFunc>(func))
      .avg();
}

} // namespace ccutils
/**
 * @file Parallel.cpp
 * @brief Parallel implementation of tabulation-based CFL reachability.
 *
 * This module provides a multi-threaded version of the tabulation algorithm
 * for computing CFL reachability. It parallelizes the computation across
 * multiple vertices, with each thread maintaining its own visited sets
 * to avoid synchronization overhead.
 *
 * Key features:
 * - Thread-safe visited sets: Each thread has its own visited tracking
 * - Work distribution: Vertices are divided among threads
 * - Two parallelization strategies:
 *   1. Thread-based: Divide vertices into chunks for each thread
 *   2. Async-based: Launch async tasks for each vertex (better load balancing)
 *
 * The parallel version maintains the same correctness guarantees as the
 * sequential Tabulation class while providing significant speedup on
 * multi-core systems.
 *
 * Thread safety: Uses thread-local visited sets to avoid contention.
 */

#include "CFL/CSIndex/FLARE/Tabulation/Parallel.h"

#include "Utils/Platform/ProgressBar.h"
#include "Utils/Parallel/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <vector>

#include <unistd.h>

namespace lotus::cfl::cs_index::flare::tabulation {

static std::atomic<bool> timeout{false};

static void alarm_handler(int) {
  timeout.store(true, std::memory_order_relaxed);
}

static std::size_t compute_parallel_tabulation_grain_size(int num_vertices,
                                                          std::size_t threads) {
  const std::size_t safe_threads = std::max<std::size_t>(1, threads);
  const std::size_t scaled_threads = std::max<std::size_t>(1, safe_threads * 4);
  const std::size_t vertex_count =
      std::max<std::size_t>(1, static_cast<std::size_t>(num_vertices));
  return std::max<std::size_t>(1, vertex_count / scaled_threads);
}

Parallel::Parallel(Graph &g)
    : vfg(g), num_threads(std::thread::hardware_concurrency()) {
  if (num_threads == 0) {
    num_threads = 4; // Default fallback
  }
}

Parallel::Parallel(Graph &g, size_t threads)
    : vfg(g), num_threads(threads ? threads : 1) {}

bool Parallel::reach(int s, int t) {
  std::set<int> visited;
  std::set<int> func_visited;

  if (visited.count(s) != 0) {
    return false;
  }

  if (s == t) {
    return true;
  }

  visited.insert(s);
  auto &edges = vfg.out_edges(s);

  for (auto successor : edges) {
    if (is_call(s, successor)) {
      // Visit the func body
      if (reach_func(successor, t, func_visited)) {
        return true;
      }
    } else {
      if (reach(successor, t)) {
        return true;
      }
    }
  }

  return false;
}

bool Parallel::reach_func(int s, int t, std::set<int> &visited) {
  if (visited.count(s) != 0) {
    return false;
  }

  if (s == t) {
    return true;
  }

  visited.insert(s);
  auto &edges = vfg.out_edges(s);

  for (auto successor : edges) {
    if (is_return(s, successor)) {
      continue;
    } else {
      if (reach_func(successor, t, visited)) {
        return true;
      }
    }
  }

  return false;
}

bool Parallel::is_call(int s, int t) { return vfg.label(s, t) > 0; }

bool Parallel::is_return(int s, int t) { return vfg.label(s, t) < 0; }

void Parallel::traverse_parallel(int s, std::set<int> &tc,
                                           std::set<int> &visited,
                                           std::set<int> &func_visited) {
  if (visited.count(s) != 0) {
    return;
  }

  if (timeout.load(std::memory_order_relaxed)) {
    return;
  }

  visited.insert(s);
  tc.insert(s);

  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_call(s, successor)) {
      // Visit the func body
      traverse_func_parallel(successor, tc, func_visited);
    } else {
      traverse_parallel(successor, tc, visited, func_visited);
    }
  }
}

void Parallel::traverse_func_parallel(int s, std::set<int> &tc,
                                                std::set<int> &visited) {
  if (visited.count(s) != 0) {
    return;
  }

  if (timeout.load(std::memory_order_relaxed)) {
    return;
  }

  visited.insert(s);
  tc.insert(s);

  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_return(s, successor)) {
      continue;
    } else {
      traverse_func_parallel(successor, tc, visited);
    }
  }
}

double Parallel::tc() {
  signal(SIGALRM, alarm_handler);
  timeout.store(false, std::memory_order_relaxed);
  alarm(3600 * 6);

  ProgressBar bar("Parallel tabulation", ProgressBar::PBS_CharacterStyle);

  double total_memory = 0;
  std::vector<std::set<int>> results(vfg.num_vertices());
  ThreadPool *pool = ThreadPool::get();

  auto compute_vertex_tc = [this, &results](int vertex) {
    if (timeout.load(std::memory_order_relaxed))
      return;

    std::set<int> local_tc;
    std::set<int> visited;
    std::set<int> func_visited;
    traverse_parallel(vertex, local_tc, visited, func_visited);
    results[static_cast<std::size_t>(vertex)] = std::move(local_tc);
  };

  if (num_threads > 1 && pool->hasWorkers()) {
    const std::size_t grain_size =
        compute_parallel_tabulation_grain_size(vfg.num_vertices(), num_threads);
    pool->parallelFor<int>(0, vfg.num_vertices(), grain_size, compute_vertex_tc);
  } else {
    for (int i = 0; i < vfg.num_vertices(); ++i)
      compute_vertex_tc(i);
  }

  total_memory =
      pool->parallelReduce<std::size_t>(0, results.size(), 16, 0.0,
                                        [&results](std::size_t index) {
                                          return static_cast<double>(
                                              results[index].size() *
                                              sizeof(int));
                                        },
                                        [](double acc, double value) {
                                          return acc + value;
                                        });

  bar.showProgress(1.0F);

  return total_memory / 1024.0 / 1024.0;
}

const char *Parallel::method() const { return "ParallelTabulate"; }

void Parallel::reset() {}

} // namespace lotus::cfl::cs_index::flare::tabulation

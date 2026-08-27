#pragma once

#include "Utils/Parallel/Cancellation.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <ostream>

namespace lotus::datalog {

enum class RunStatus {
  Completed,
  Cancelled,
};

struct ExecutionStats {
  std::size_t rule_evaluations = 0;
  std::size_t tuples_scanned = 0;
  std::size_t index_lookups = 0;
  std::size_t inserted_facts = 0;
  std::size_t fixpoint_iterations = 0;
  std::size_t planned_reorders = 0;
  std::size_t parallel_tasks = 0;
  std::size_t parallel_rule_tasks = 0;
  std::size_t parallel_merge_tasks = 0;
  std::size_t parallel_aggregate_tasks = 0;
  std::size_t scc_count = 0;
  std::size_t relation_count = 0;
  std::size_t total_facts = 0;
  std::size_t peak_delta = 0;
  std::size_t index_count = 0;
  std::size_t index_entries = 0;
  std::size_t index_memory_bytes = 0;
  std::size_t tuple_memory_bytes = 0;
  std::size_t uniqueness_memory_bytes = 0;
  std::size_t base_memory_bytes = 0;
  std::size_t head_derivations = 0;
  std::size_t local_unique_candidates = 0;
  std::size_t global_unique_candidates = 0;
  std::size_t incremental_sccs = 0;
  std::size_t rebuilt_sccs = 0;
  std::size_t base_delta_facts = 0;
};

class Scheduler {
public:
  virtual ~Scheduler() = default;
  // Implementations must report at least one worker. CompiledProgram::run()
  // rejects injected schedulers that violate this contract.
  virtual std::size_t workerCount() const = 0;
  virtual void
  parallelFor(std::size_t task_count,
              const std::function<void(std::size_t)> &function) = 0;
};

class SerialScheduler final : public Scheduler {
public:
  std::size_t workerCount() const override { return 1; }
  void parallelFor(std::size_t task_count,
                   const std::function<void(std::size_t)> &function) override;
};

class ThreadScheduler final : public Scheduler {
public:
  explicit ThreadScheduler(std::size_t worker_count);
  ~ThreadScheduler() override;

  ThreadScheduler(const ThreadScheduler &) = delete;
  ThreadScheduler &operator=(const ThreadScheduler &) = delete;

  std::size_t workerCount() const override { return worker_count_; }
  void parallelFor(std::size_t task_count,
                   const std::function<void(std::size_t)> &function) override;

private:
  struct Impl;

  std::size_t worker_count_ = 1;
  std::unique_ptr<Impl> impl_;
};

struct ExecutionOptions {
  std::size_t worker_count = 1;
  std::size_t parallel_grain_size = 256;
  Scheduler *scheduler = nullptr;
  lotus::CancellationToken cancellation;
  bool trace_scc = false;
  bool trace_rule = false;
  bool trace_delta = false;
  bool collect_profile = false;
  std::ostream *trace_stream = nullptr;
};

} // namespace lotus::datalog

#include "Solvers/Datalog/Runtime/Scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lotus::datalog {
namespace {

struct TaskGroup {
  explicit TaskGroup(std::size_t count) : remaining(count) {}

  std::atomic<std::size_t> remaining;
  std::mutex mutex;
  std::condition_variable complete;
  std::exception_ptr failure;
};

} // namespace

struct ThreadScheduler::Impl {
  explicit Impl(std::size_t worker_count) {
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
      workers.emplace_back([this] { workerLoop(); });
    }
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    for (std::thread &worker : workers)
      worker.join();
  }

  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      tasks.push_back(std::move(task));
    }
    ready.notify_one();
  }

  bool isWorkerThread() const { return current == this; }

private:
  void workerLoop() {
    current = this;
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return stopping || !tasks.empty(); });
        if (stopping && tasks.empty())
          break;
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task();
    }
    current = nullptr;
  }

  std::vector<std::thread> workers;
  std::deque<std::function<void()>> tasks;
  std::mutex mutex;
  std::condition_variable ready;
  bool stopping = false;

  static thread_local const Impl *current;
};

thread_local const ThreadScheduler::Impl *ThreadScheduler::Impl::current =
    nullptr;

void SerialScheduler::parallelFor(
    std::size_t task_count, const std::function<void(std::size_t)> &function) {
  for (std::size_t task = 0; task < task_count; ++task)
    function(task);
}

ThreadScheduler::ThreadScheduler(std::size_t worker_count)
    : worker_count_(std::max<std::size_t>(1, worker_count)),
      impl_(std::make_unique<Impl>(worker_count_)) {}

ThreadScheduler::~ThreadScheduler() = default;

void ThreadScheduler::parallelFor(
    std::size_t task_count, const std::function<void(std::size_t)> &function) {
  if (task_count == 0)
    return;
  const std::size_t workers = std::min(worker_count_, task_count);
  if (workers <= 1 || impl_->isWorkerThread()) {
    for (std::size_t task = 0; task < task_count; ++task)
      function(task);
    return;
  }

  auto group = std::make_shared<TaskGroup>(workers);
  auto next_task = std::make_shared<std::atomic<std::size_t>>(0);
  for (std::size_t worker = 0; worker < workers; ++worker) {
    impl_->submit([group, next_task, task_count, &function] {
      while (true) {
        const std::size_t task = next_task->fetch_add(1);
        if (task >= task_count)
          break;
        try {
          function(task);
        } catch (...) {
          std::lock_guard<std::mutex> lock(group->mutex);
          if (!group->failure)
            group->failure = std::current_exception();
        }
      }
      if (group->remaining.fetch_sub(1) == 1)
        group->complete.notify_one();
    });
  }

  std::unique_lock<std::mutex> lock(group->mutex);
  group->complete.wait(lock, [&] { return group->remaining.load() == 0; });
  if (group->failure)
    std::rethrow_exception(group->failure);
}

} // namespace lotus::datalog

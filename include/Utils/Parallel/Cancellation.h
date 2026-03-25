#pragma once

#include <atomic>
#include <memory>
#include <stdexcept>

namespace lotus {

class TaskCancelledError : public std::runtime_error {
public:
  TaskCancelledError()
      : std::runtime_error("parallel task was cancelled before execution") {}
};

class CancellationToken {
private:
  std::shared_ptr<std::atomic<bool>> m_state;

  explicit CancellationToken(const std::shared_ptr<std::atomic<bool>> &State)
      : m_state(State) {}

  friend class CancellationSource;

public:
  CancellationToken() = default;

  bool isCancelled() const {
    return m_state && m_state->load(std::memory_order_acquire);
  }

  explicit operator bool() const { return static_cast<bool>(m_state); }
};

class CancellationSource {
private:
  std::shared_ptr<std::atomic<bool>> m_state;

public:
  CancellationSource() : m_state(std::make_shared<std::atomic<bool>>(false)) {}

  void cancel() const {
    if (m_state)
      m_state->store(true, std::memory_order_release);
  }

  bool isCancelled() const {
    return m_state && m_state->load(std::memory_order_acquire);
  }

  CancellationToken token() const { return CancellationToken(m_state); }
};

} // namespace lotus

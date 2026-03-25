#pragma once

#include <cstddef>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <type_traits>

#include <llvm/Support/raw_ostream.h>

namespace mkint {

// Log level enum
enum class LogLevel {
  DEBUG,   // Debugging information (very verbose)
  INFO,    // Normal informational messages
  WARNING, // Warning messages
  ERROR,   // Error messages
  NONE     // No logging (quiet mode)
};

// Configuration structure for logging
struct LogConfig {
  bool quiet = false;       // Suppress most log output
  bool useStderr = false;   // Redirect logs to stderr instead of stdout
  std::string logFile = ""; // Log to file (empty = no file logging)
  LogLevel logLevel = LogLevel::INFO; // Current log level (default: INFO)
};

// Forward declaration for nullstream
class nullstream;

// Main Logger class - Singleton pattern
class Logger {
public:
  static Logger &getInstance();

  // No copy or move
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  // Configure the logger
  void configure(LogConfig config);

  // Get current config
  const LogConfig &getConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
  }

  // Get the appropriate output stream based on current config
  std::ostream &getStream();

private:
  Logger() = default;

  LogConfig m_config;
  mutable std::mutex m_mutex;
  std::unique_ptr<std::ofstream> m_fileStream;
  std::reference_wrapper<std::ostream> m_currentStream{std::cout};
  bool m_streamInitialized = false;
};

namespace detail {
// C++14 compatible void_t implementation
template <typename...> struct make_void {
  typedef void type;
};
template <typename... Ts> using void_t = typename make_void<Ts...>::type;

// Use struct member instead of variable template (C++17)
template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<
    T, void_t<decltype(std::declval<std::ostream &>() << std::declval<T>())>>
    : std::true_type {};

// Removed C++17 variable template

class log_wrapper {
public:
  // Replace fold expression with C++14 compatible code
  template <typename Arg> void write_arg(std::ostream &stream, Arg &&arg) {
    stream << std::forward<Arg>(arg);
  }

  template <typename... Args>
  log_wrapper(std::ostream &stream, Args &&...args) : m_stream(stream) {
    // Use C++14 compatible way to unpack arguments
    int dummy[] = {0, (write_arg(m_stream, std::forward<Args>(args)), 0)...};
    (void)dummy; // Suppress unused variable warning
  }

  log_wrapper(log_wrapper &&wrapper) noexcept;
  log_wrapper(log_wrapper &) = delete;

  // Replace std::string_view with const std::string&
  log_wrapper &&operator<<(const std::string &v);

  template <typename T>
  typename std::enable_if<std::is_convertible<T, std::string>::value,
                          log_wrapper &&>::type
  operator<<(const T &v) {
    return operator<<(std::string(v));
  }

  template <typename T>
  typename std::enable_if<!std::is_convertible<T, std::string>::value &&
                              is_streamable<T>::value,
                          log_wrapper &&>::type
  operator<<(const T &v) {
    m_stream << v;
    m_last_was_newline = false;
    return std::move(*this);
  }

  template <typename T>
  typename std::enable_if<!std::is_convertible<T, std::string>::value &&
                              !is_streamable<T>::value,
                          log_wrapper &&>::type
  operator<<(const T &v) {
    std::string str;
    llvm::raw_string_ostream(str) << v;
    return operator<<(str);
  }

  log_wrapper &&abort_at_deconstruct();

  ~log_wrapper();

private:
  std::ostream &m_stream;
  bool m_last_was_newline = false;
  bool m_abort_at_deconstruct = false;
  bool m_stop = false;
}; // class log_wrapper
} // namespace detail

detail::log_wrapper log();
detail::log_wrapper debug();
detail::log_wrapper warn();
detail::log_wrapper error();
detail::log_wrapper check(bool cond, bool abort, const std::string &prompt,
                          const std::string &file, size_t line);

} // namespace mkint

#define MKINT_LOG() mkint::log()
#define MKINT_DEBUG() mkint::debug()
#define MKINT_WARN() mkint::warn()
#define MKINT_ERROR() mkint::error()

#define MKINT_CHECK_1(cond) mkint::check(cond, true, #cond, __FILE__, __LINE__)
#define MKINT_CHECK_2(cond, abort)                                             \
  mkint::check(cond, abort, #cond, __FILE__, __LINE__)

#define MKINT_CHECK_X(x, cond, abort, FUNC, ...) FUNC
#define MKINT_CHECK(...)                                                       \
  MKINT_CHECK_X(, ##__VA_ARGS__, MKINT_CHECK_2(__VA_ARGS__),                   \
                MKINT_CHECK_1(__VA_ARGS__))

#define MKINT_CHECK_RELAX(cond) MKINT_CHECK_2(cond, false)
#define MKINT_CHECK_ABORT(cond) MKINT_CHECK_2(cond, true)

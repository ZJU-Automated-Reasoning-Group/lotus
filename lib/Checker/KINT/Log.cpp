#include "Checker/KINT/Log.h"

#include "Utils/Types/range.h"

// #include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>

// Prompt and style constants
constexpr const char *LOG_PROMPT = "[LOG]";
constexpr auto LOG_STYLE_FG = rang::fg::green;
constexpr auto LOG_STYLE_BG = rang::bg::gray;

constexpr const char *WARN_PROMPT = "[WARN]";
constexpr auto WARN_STYLE_FG = rang::fg::yellow;

constexpr const char *ERROR_PROMPT = "[ERROR]";
constexpr auto ERROR_STYLE_FG = rang::fg::red;

constexpr const char *CHECK_PROMPT = "[CHECK]";
constexpr auto CHECK_STYLE_FG = rang::fg::red;
constexpr auto CHECK_STYLE_BG = rang::bg::gray;

constexpr const char *DEBUG_PROMPT = "[DEBUG]";
constexpr auto DEBUG_STYLE_FG = rang::fg::black;
constexpr auto DEBUG_STYLE_BG = rang::bg::yellow;

namespace mkint {

// Null output stream for quiet mode
class nullstream : public std::ostream {
public:
  nullstream() : std::ostream(nullptr) {}
  nullstream(const nullstream &) : std::ostream(nullptr) {}
};

template <typename T> const nullstream &operator<<(nullstream &os, const T &) {
  return os;
}

// Static null stream instance
static nullstream s_null_stream;

// Global Logger instance
Logger &Logger::getInstance() {
  static Logger instance;
  return instance;
}

void Logger::configure(LogConfig config) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_config = std::move(config);
  // Close any existing file stream
  if (m_fileStream) {
    m_fileStream->close();
    m_fileStream.reset();
  }

  // Create new file stream if needed
  if (!m_config.logFile.empty()) {
    m_fileStream = std::make_unique<std::ofstream>(m_config.logFile);
    if (!m_fileStream->is_open()) {
      std::cerr << "Error: Unable to open log file: " << m_config.logFile
                << "\n";
      m_fileStream.reset();
    }
  }

  // Update the current stream
  if (m_config.quiet) {
    m_currentStream = std::ref(s_null_stream);
  } else if (m_fileStream && m_fileStream->is_open()) {
    m_currentStream = std::ref(*m_fileStream);
  } else if (m_config.useStderr) {
    m_currentStream = std::ref(std::cerr);
  } else {
    m_currentStream = std::ref(std::cout);
  }

  m_streamInitialized = true;
}

std::ostream &Logger::getStream() {
  std::lock_guard<std::mutex> lock(m_mutex);

  // Make sure we're initialized
  if (!m_streamInitialized) {
    if (m_config.quiet) {
      m_currentStream = std::ref(s_null_stream);
    } else if (m_config.useStderr) {
      m_currentStream = std::ref(std::cerr);
    } else {
      m_currentStream = std::ref(std::cout);
    }
    m_streamInitialized = true;
  }

  return m_currentStream.get();
}

detail::log_wrapper::log_wrapper(log_wrapper &&wrapper) noexcept
    : m_stream(wrapper.m_stream),
      m_last_was_newline(wrapper.m_last_was_newline),
      m_abort_at_deconstruct(wrapper.m_abort_at_deconstruct) {
  wrapper.m_last_was_newline = false;
  wrapper.m_abort_at_deconstruct = false;
  wrapper.m_stop = true;
}

detail::log_wrapper &&detail::log_wrapper::operator<<(const std::string &v) {
  if (m_stop) {
    return std::move(*this);
  }

  m_stream << v;
  if (!v.empty())
    m_last_was_newline = (v.back() == '\n');
  return std::move(*this);
}

detail::log_wrapper &&detail::log_wrapper::abort_at_deconstruct() {
  m_abort_at_deconstruct = true;
  return std::move(*this);
}

detail::log_wrapper::~log_wrapper() {
  if (m_stop)
    return;

  if (!m_last_was_newline) {
    m_stream << "\n";
  }

  if (m_abort_at_deconstruct) {
    std::abort();
  }
}

detail::log_wrapper log() {
  auto &logger = Logger::getInstance();
  const auto &config = logger.getConfig();

  // Only show log messages if level is INFO or lower
  if (config.quiet || config.logLevel > LogLevel::INFO) {
    return detail::log_wrapper(s_null_stream);
  }
  return detail::log_wrapper(logger.getStream(), LOG_STYLE_FG, LOG_STYLE_BG,
                             LOG_PROMPT, rang::style::reset, '\t');
}

detail::log_wrapper debug() {
  auto &logger = Logger::getInstance();
  const auto &config = logger.getConfig();

  // Only show debug messages if level is DEBUG
  if (config.quiet || config.logLevel > LogLevel::DEBUG) {
    return detail::log_wrapper(s_null_stream);
  }
  return detail::log_wrapper(logger.getStream(), DEBUG_STYLE_FG, DEBUG_STYLE_BG,
                             DEBUG_PROMPT, rang::style::reset, '\t');
}

detail::log_wrapper warn() {
  auto &logger = Logger::getInstance();
  const auto &config = logger.getConfig();

  // Only show warning messages if level is WARNING or lower
  if (config.quiet || config.logLevel > LogLevel::WARNING) {
    return detail::log_wrapper(s_null_stream);
  }
  return detail::log_wrapper(logger.getStream(), WARN_STYLE_FG, WARN_PROMPT,
                             rang::style::reset, '\t');
}

detail::log_wrapper error() {
  auto &logger = Logger::getInstance();
  const auto &config = logger.getConfig();

  // Only show error messages if level is ERROR or lower
  if (config.quiet || config.logLevel > LogLevel::ERROR) {
    return detail::log_wrapper(s_null_stream);
  }
  return detail::log_wrapper(logger.getStream(), ERROR_STYLE_FG, ERROR_PROMPT,
                             rang::style::reset, '\t');
}

detail::log_wrapper check(bool cond, bool abort, const std::string &prompt,
                          const std::string &file, size_t line) {
  if (!cond) {
    auto &logger = Logger::getInstance();
    const auto &config = logger.getConfig();

    // Suppress the diagnostic message only when the log level is set to
    // NONE (i.e. completely silent).  Using >= would also suppress at
    // LogLevel::ERROR which is wrong — errors should always be shown
    // unless the user explicitly requests NONE.
    if (config.quiet || config.logLevel > LogLevel::NONE) {
      if (abort)
        std::abort();
      return detail::log_wrapper(s_null_stream);
    }

    auto wrapper = detail::log_wrapper(
        logger.getStream(), CHECK_STYLE_FG, CHECK_STYLE_BG, CHECK_PROMPT,
        rang::style::reset, ' ', rang::fg::yellow, prompt, " at ", file, ':',
        line, '\t', rang::style::reset);

    if (abort)
      return wrapper.abort_at_deconstruct();
    else
      return wrapper;
  } else {
    return detail::log_wrapper(s_null_stream);
  }
}

} // namespace mkint
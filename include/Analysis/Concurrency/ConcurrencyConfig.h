/**
 * @file ConcurrencyConfig.h
 * @brief Configuration for Concurrency Analysis Threading Models
 *
 * This file provides configuration options for enabling/disabling different
 * threading models in the concurrency analysis. This allows users to focus
 * analysis on specific concurrency primitives (e.g., only pthread, only MPI)
 * or enable all models for comprehensive analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#pragma once

#include <cstdint>

namespace concurrency {

/**
 * @brief Configuration options for threading models
 *
 * Each flag enables/disables recognition and analysis of a specific
 * threading model. By default, all models are enabled for comprehensive
 * analysis, but users can disable specific models to:
 * - Improve performance (skip unnecessary pattern matching)
 * - Focus analysis on specific concurrency primitives
 * - Avoid false positives from unsupported models
 */
enum class ThreadingModelOptions : uint32_t {
  None = 0,
  EnablePthread = 1 << 0,         ///< Enable POSIX threads (pthread) analysis
  EnableCpp11 = 1 << 1,           ///< Enable C++11/17/20 threading analysis
  EnableOpenMP = 1 << 2,          ///< Enable OpenMP analysis
  EnableMPI = 1 << 3,             ///< Enable MPI (Message Passing Interface) analysis
  EnableLinuxKernel = 1 << 4,     ///< Enable Linux kernel concurrency primitives
  All = ~0U                       ///< Enable all threading models (default)
};

/**
 * @brief Configuration for concurrency analysis threading models
 */
struct ConcurrencyConfig {
  ConcurrencyConfig() : m_options(static_cast<uint32_t>(ThreadingModelOptions::All)) {}

  explicit ConcurrencyConfig(ThreadingModelOptions options)
      : m_options(static_cast<uint32_t>(options)) {}

  /// Enable/disable POSIX threads (pthread) analysis
  bool enable_pthread() const {
    return (m_options & static_cast<uint32_t>(ThreadingModelOptions::EnablePthread)) != 0;
  }
  void set_enable_pthread(bool enable = true) {
    if (enable)
      m_options |= static_cast<uint32_t>(ThreadingModelOptions::EnablePthread);
    else
      m_options &= ~static_cast<uint32_t>(ThreadingModelOptions::EnablePthread);
  }

  /// Enable/disable C++11/17/20 threading analysis
  bool enable_cpp11() const {
    return (m_options & static_cast<uint32_t>(ThreadingModelOptions::EnableCpp11)) != 0;
  }
  void set_enable_cpp11(bool enable = true) {
    if (enable)
      m_options |= static_cast<uint32_t>(ThreadingModelOptions::EnableCpp11);
    else
      m_options &= ~static_cast<uint32_t>(ThreadingModelOptions::EnableCpp11);
  }

  /// Enable/disable OpenMP analysis
  bool enable_openmp() const {
    return (m_options & static_cast<uint32_t>(ThreadingModelOptions::EnableOpenMP)) != 0;
  }
  void set_enable_openmp(bool enable = true) {
    if (enable)
      m_options |= static_cast<uint32_t>(ThreadingModelOptions::EnableOpenMP);
    else
      m_options &= ~static_cast<uint32_t>(ThreadingModelOptions::EnableOpenMP);
  }

  /// Enable/disable MPI analysis
  bool enable_mpi() const {
    return (m_options & static_cast<uint32_t>(ThreadingModelOptions::EnableMPI)) != 0;
  }
  void set_enable_mpi(bool enable = true) {
    if (enable)
      m_options |= static_cast<uint32_t>(ThreadingModelOptions::EnableMPI);
    else
      m_options &= ~static_cast<uint32_t>(ThreadingModelOptions::EnableMPI);
  }

  /// Enable/disable Linux kernel concurrency primitives analysis
  bool enable_linux_kernel() const {
    return (m_options & static_cast<uint32_t>(ThreadingModelOptions::EnableLinuxKernel)) != 0;
  }
  void set_enable_linux_kernel(bool enable = true) {
    if (enable)
      m_options |= static_cast<uint32_t>(ThreadingModelOptions::EnableLinuxKernel);
    else
      m_options &= ~static_cast<uint32_t>(ThreadingModelOptions::EnableLinuxKernel);
  }

  /// Get raw options value
  uint32_t get_options() const { return m_options; }

  /// Set raw options value
  void set_options(uint32_t options) { m_options = options; }

private:
  uint32_t m_options;
};

} // namespace concurrency

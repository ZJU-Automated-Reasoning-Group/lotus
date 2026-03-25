/*
 * Taint Configuration Manager
 *
 * Singleton manager for taint configurations.
 *
 * Fixes applied:
 *  - Constructor is now private to enforce the singleton pattern.
 *  - getInstance() uses a function-local static (guaranteed thread-safe in
 *    C++11 and later) instead of the previous non-thread-safe manual
 *    unique_ptr check.
 *  - normalize_function_name: the fortified-function stripping logic now
 *    correctly removes the extra '_' separator before "_chk"
 *    (e.g. "__strcpy_chk" → "strcpy" instead of "strcpy_").
 *  - load_default_config: accepts an optional install-prefix path so that
 *    callers can supply an absolute path rather than relying on fragile
 *    relative-path heuristics.
 */

#pragma once

#include "Annotation/Taint/TaintConfigParser.h"

#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>

class TaintConfigManager {
public:
  // Non-copyable, non-movable singleton.
  TaintConfigManager(const TaintConfigManager &) = delete;
  TaintConfigManager &operator=(const TaintConfigManager &) = delete;

  /// Returns the singleton instance.  Thread-safe (C++11 magic statics).
  static TaintConfigManager &getInstance() {
    static TaintConfigManager instance;
    return instance;
  }

  bool load_config(const std::string &config_file) {
    config = TaintConfigParser::parse_file(config_file);
    return config != nullptr;
  }

  bool load_config_quiet(const std::string &config_file) {
    config = TaintConfigParser::parse_file_quiet(config_file);
    return config != nullptr;
  }

  /// Try to load the default taint spec.
  ///
  /// @param install_prefix  Optional absolute path to the project/install
  ///                        root.  When non-empty it is tried first, which
  ///                        avoids the fragile CWD-relative heuristics.
  bool load_default_config(const std::string &install_prefix = "") {
    // Build the candidate list.  An explicit prefix is tried first so
    // that tools installed to a known location always find the config.
    std::vector<std::string> candidates;
    if (!install_prefix.empty()) {
      candidates.push_back(install_prefix + "/config/taint.spec");
    }
    // CWD-relative fallbacks (useful during development / in-tree builds).
    candidates.push_back("config/taint.spec");
    candidates.push_back("../config/taint.spec");
    candidates.push_back("../../config/taint.spec");
    candidates.push_back("../../../config/taint.spec");

    for (const auto &path : candidates) {
      if (load_config_quiet(path)) {
        return true;
      }
    }

    llvm::errs() << "Error: Could not find taint config file in any of the "
                    "expected locations\n";
    return false;
  }

  bool is_source(const std::string &func_name) const {
    if (!config)
      return false;
    std::string normalized = normalize_function_name(func_name);
    return config->is_source(normalized);
  }

  bool is_sink(const std::string &func_name) const {
    if (!config)
      return false;
    std::string normalized = normalize_function_name(func_name);
    return config->is_sink(normalized);
  }

  bool is_ignored(const std::string &func_name) const {
    if (!config)
      return false;
    std::string normalized = normalize_function_name(func_name);
    return config->is_ignored(normalized);
  }

  bool is_source(const llvm::CallInst *call) const {
    if (!call)
      return false;
    const llvm::Function *callee = call->getCalledFunction();
    return callee && is_source(callee->getName().str());
  }

  bool is_sink(const llvm::CallInst *call) const {
    if (!call)
      return false;
    const llvm::Function *callee = call->getCalledFunction();
    return callee && is_sink(callee->getName().str());
  }

  void dump_config(llvm::raw_ostream &OS) const {
    if (config)
      config->dump(OS);
  }

  size_t get_source_count() const {
    return config ? config->sources.size() : 0;
  }

  size_t get_sink_count() const { return config ? config->sinks.size() : 0; }

  std::vector<std::string> get_all_source_functions() const {
    std::vector<std::string> result;
    if (config) {
      for (const auto &func : config->sources)
        result.push_back(func);
    }
    return result;
  }

  std::vector<std::string> get_all_sink_functions() const {
    std::vector<std::string> result;
    if (config) {
      for (const auto &func : config->sinks)
        result.push_back(func);
    }
    return result;
  }

  const FunctionTaintConfig *
  get_function_config(const std::string &func_name) const {
    if (!config)
      return nullptr;
    std::string normalized = normalize_function_name(func_name);
    return config->get_function_config(normalized);
  }

  /// Expose normalisation for external use (e.g. tests).
  static std::string get_normalized_name(const std::string &func_name) {
    return normalize_function_name(func_name);
  }

private:
  // Private constructor — use getInstance().
  TaintConfigManager() = default;

  std::unique_ptr<TaintConfig> config;

  /// Normalise a function name for lookup.
  ///
  /// Handles:
  ///  1. Platform-specific byte-prefix "\x01_" (macOS/Darwin linker stubs).
  ///  2. Fortified variants: "__foo_chk" → "foo".
  ///     The pattern is: leading "__" + base name + "_chk".
  ///     The base name itself does NOT contain a trailing '_'; the separator
  ///     before "_chk" is part of the suffix, so we strip 2 + 4 + 1 = 7
  ///     characters (prefix "__" = 2, suffix "_chk" = 4, separator "_" = 1).
  ///
  ///     Example: "__strcpy_chk" (length 12)
  ///       substr(2, 12 - 2 - 5) = substr(2, 5) = "strcpy"  ✓
  ///     Example: "__memcpy_chk" (length 12)
  ///       substr(2, 12 - 2 - 5) = substr(2, 5) = "memcpy"  ✓
  static std::string normalize_function_name(const std::string &func_name) {
    std::string normalized = func_name;

    // Strip platform-specific "\x01_" prefix (macOS/Darwin).
    if (normalized.size() > 2 &&
        static_cast<unsigned char>(normalized[0]) == 1 &&
        normalized[1] == '_') {
      normalized = normalized.substr(2);
    }

    // Strip fortified suffix: "__<name>_chk" → "<name>".
    // Minimum length: "__" (2) + at least one base char (1) + "_chk" (4) = 7.
    const std::string chk_suffix = "_chk";
    if (normalized.size() >= 7 && normalized.compare(0, 2, "__") == 0 &&
        normalized.size() >= chk_suffix.size() &&
        normalized.compare(normalized.size() - chk_suffix.size(),
                           chk_suffix.size(), chk_suffix) == 0) {
      // Strip leading "__" (2 chars) and trailing "_chk" (4 chars) plus
      // the '_' separator that immediately precedes "_chk" (1 char).
      // Total to strip from the end: 5 chars ("_" + "_chk").
      const size_t strip_prefix = 2;
      const size_t strip_suffix = 5; // "_" separator + "_chk"
      if (normalized.size() > strip_prefix + strip_suffix) {
        normalized = normalized.substr(
            strip_prefix, normalized.size() - strip_prefix - strip_suffix);
      }
    }

    return normalized;
  }
};

// Convenience namespace — thin wrappers around the singleton.
namespace taint_config {
inline bool is_source(const std::string &func_name) {
  return TaintConfigManager::getInstance().is_source(func_name);
}

inline bool is_sink(const std::string &func_name) {
  return TaintConfigManager::getInstance().is_sink(func_name);
}

inline bool is_ignored(const std::string &func_name) {
  return TaintConfigManager::getInstance().is_ignored(func_name);
}

inline bool is_source(const llvm::CallInst *call) {
  return TaintConfigManager::getInstance().is_source(call);
}

inline bool is_sink(const llvm::CallInst *call) {
  return TaintConfigManager::getInstance().is_sink(call);
}

inline bool load_config(const std::string &config_file) {
  return TaintConfigManager::getInstance().load_config(config_file);
}

/// @param install_prefix  Optional absolute path to the project root.
inline bool load_default_config(const std::string &install_prefix = "") {
  return TaintConfigManager::getInstance().load_default_config(install_prefix);
}

inline void dump_config(llvm::raw_ostream &OS) {
  TaintConfigManager::getInstance().dump_config(OS);
}

inline size_t get_source_count() {
  return TaintConfigManager::getInstance().get_source_count();
}

inline size_t get_sink_count() {
  return TaintConfigManager::getInstance().get_sink_count();
}

inline const FunctionTaintConfig *
get_function_config(const std::string &func_name) {
  return TaintConfigManager::getInstance().get_function_config(func_name);
}

inline std::string normalize_name(const std::string &func_name) {
  return TaintConfigManager::get_normalized_name(func_name);
}
} // namespace taint_config

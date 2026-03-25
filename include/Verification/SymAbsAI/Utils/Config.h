#pragma once

#include "Verification/SymAbsAI/Utils/Utils.h"

#include <map>
#include <string>

#include <llvm/ADT/StringRef.h>

namespace symabs_ai {
// Forward declaration
class DomainConstructor;

namespace configparser {
/**
 * Provides configuration (Python binding disabled for Lotus integration).
 */
class Config {
private:
  static constexpr const char *ENV_VAR = "SYMBOLIC_ABSTRACTION_CONFIG";

  // Stubbed out - Python binding disabled
  shared_ptr<std::map<std::string, std::string>> ConfigDict_;

public:
  /**
   * Creates a `Config` object based on reasonable defaults.
   */
  Config(llvm::StringRef file_name);

  /**
   * Creates a `Config` object based on reasonable defaults.
   */
  Config();

  template <typename T>
  T get(const char *module, const char *key, T default_value) const;

  void set(const char *module, const char *key, llvm::StringRef value);
  void set(const char *module, const char *key, int value);
  void set(const char *module, const char *key, bool value);
};
} // namespace configparser
} // namespace symabs_ai

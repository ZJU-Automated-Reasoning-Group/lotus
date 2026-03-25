#pragma once

#include <memory>
#include <string>
#include <vector>

namespace lotus {
namespace verification {
namespace backend {

enum class PropertyClass {
  Reachability,
  MemSafety,
  Overflow,
  Termination,
  Unknown
};

enum class VerificationResult {
  True,    // Property holds (no error found)
  False,   // Property violated (error found)
  Unknown, // Could not determine
  Error,   // Verification tool error
  Timeout  // Verification timed out
};

struct VerificationTask {
  std::string inputBitcode;
  PropertyClass property = PropertyClass::Unknown;
  unsigned timeoutSeconds = 0;
  std::vector<std::string> extraArgs;
};

struct VerificationResultInfo {
  VerificationResult result = VerificationResult::Unknown;
  std::string message;
  std::string errorTrace; // If result is False, may contain error trace
  int exitCode = -1;

  bool isSafe() const { return result == VerificationResult::True; }
  bool hasError() const { return result == VerificationResult::False; }
  bool isUnknown() const { return result == VerificationResult::Unknown; }
};

class IBackend {
public:
  virtual ~IBackend() = default;
  virtual const char *name() const = 0;
  virtual bool supports(PropertyClass property) const = 0;
  virtual std::vector<std::string>
  buildCommand(const VerificationTask &task) const = 0;

  // Parse backend output and normalize to standard result format
  virtual VerificationResultInfo parseResult(const std::string &output,
                                             int exitCode) const = 0;
};

class BackendRegistry {
public:
  static BackendRegistry &instance();

  std::vector<std::string> availableBackends() const;
  std::unique_ptr<IBackend> create(const std::string &name) const;
  std::vector<std::string> recommend(PropertyClass property) const;

private:
  BackendRegistry() = default;
};

PropertyClass parsePropertyClass(const std::string &name);
std::string toString(PropertyClass property);

VerificationResult parseResultFromString(const std::string &str);
std::string toString(VerificationResult result);

} // namespace backend
} // namespace verification
} // namespace lotus

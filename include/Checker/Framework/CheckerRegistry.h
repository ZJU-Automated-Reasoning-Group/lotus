/** @file CheckerRegistry.h @brief Registry for registering and instantiating
 * checker analyses. */
#pragma once

#include "Checker/Framework/CheckerTypes.h"

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

namespace lotus::checker {

struct NativeCheckDescriptor {
  llvm::StringRef id;
  llvm::StringRef title;
  bool default_enabled = true;
};

struct CheckerDescriptor {
  CheckerMetadata metadata;
  RuleKind rule_kind = RuleKind::Native;
  std::vector<CheckerCapability> capabilities;
  bool executable = false;
  std::optional<CheckerSpec> spec;

  bool isDeclarative() const {
    return metadata.engine == EngineKind::Declarative && spec.has_value();
  }
};

class CheckerRegistry {
public:
  llvm::Error registerDeclarative(const CheckerSpec &spec);
  llvm::Error registerNative(const CheckerMetadata &metadata,
                             RuleKind rule_kind,
                             std::vector<CheckerCapability> capabilities,
                             bool executable = false);

  llvm::Expected<const CheckerDescriptor *> findById(llvm::StringRef id) const;
  std::vector<const CheckerDescriptor *> list() const;
  std::vector<const CheckerDescriptor *>
  select(llvm::StringRef category, std::optional<EngineKind> engine) const;

private:
  std::vector<CheckerDescriptor> descriptors_;
};

llvm::Error registerBuiltinNativeCheckers(CheckerRegistry &registry);
llvm::ArrayRef<NativeCheckDescriptor> getBuiltinNativeChecks(EngineKind engine);

} // namespace lotus::checker

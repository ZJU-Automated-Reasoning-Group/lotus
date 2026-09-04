#include "Checker/Framework/CheckerRegistry.h"

#include <llvm/Support/Error.h>

using namespace llvm;

namespace lotus::checker {

namespace {

constexpr NativeCheckDescriptor AE_CHECKS[] = {
    {"buffer-overflow", "Buffer overflow"},
    {"null-deref", "Null pointer dereference"},
    {"use-after-free", "Use after free"},
    {"invalid-free", "Invalid free"},
    {"memory-leak", "Memory leak"},
};
constexpr NativeCheckDescriptor SABER_CHECKS[] = {
    {"memory-leak", "Memory leak"},
    {"double-free", "Double free"},
    {"file-leak", "File descriptor leak"},
};
constexpr NativeCheckDescriptor PULSE_CHECKS[] = {
    {"null-deref", "Null pointer dereference"},
    {"use-after-free", "Use after free"},
    {"out-of-bounds", "Out-of-bounds access"},
    {"invalid-free", "Invalid free"},
    {"uninitialized-read", "Uninitialized read"},
    {"taint-flow", "Tainted data flow"},
    {"unnecessary-copy", "Unnecessary copy"},
    {"stack-address-escape", "Stack address escape"},
    {"const-refable-parameter", "Parameter can be passed by const reference"},
};
constexpr NativeCheckDescriptor KINT_CHECKS[] = {
    {"int-overflow", "Integer overflow"},
    {"div-by-zero", "Division by zero"},
    {"bad-shift", "Invalid shift"},
    {"array-oob", "Array index out of bounds"},
    {"dead-branch", "Dead branch"},
};
constexpr NativeCheckDescriptor TAINT_CHECKS[] = {
    {"taint-flow", "Tainted data flow"},
};
constexpr NativeCheckDescriptor FITX_CHECKS[] = {
    {"double-free", "Double free"},
    {"double-lock", "Double lock"},
    {"double-unlock", "Double unlock"},
    {"memory-leak", "Memory leak"},
    {"null-deref", "Null pointer dereference"},
    {"use-after-free", "Use after free"},
    {"use-before-init", "Use before initialization"},
    {"ref-count", "Reference count misuse"},
    {"ref-uncount", "Reference uncount misuse"},
};
constexpr NativeCheckDescriptor CONCURRENCY_CHECKS[] = {
    {"data-race", "Data race"},
    {"deadlock", "Deadlock"},
    {"atomicity", "Atomicity violation"},
    {"condvar", "Condition-variable misuse"},
    {"lock-mismatch", "Lock/unlock mismatch"},
    {"openmp", "OpenMP misuse"},
    {"mpi", "MPI misuse"},
    {"cuda", "CUDA misuse"},
};
constexpr NativeCheckDescriptor SYMEX_CHECKS[] = {
    {"buffer-overflow", "Buffer overflow"},
    {"div-by-zero", "Division by zero"},
    {"int-overflow", "Integer overflow"},
    {"int-underflow", "Integer underflow"},
    {"null-deref", "Null pointer dereference"},
    {"signed-int-overflow", "Signed integer overflow"},
    {"signed-int-underflow", "Signed integer underflow"},
    {"shift-overflow", "Shift overflow"},
    {"array-oob", "Array index out of bounds"},
    {"uninitialized-read", "Uninitialized read"},
    {"use-after-free", "Use after free"},
    {"double-free", "Double free"},
    {"negative-array-index", "Negative array index"},
    {"int-truncation", "Integer truncation"},
};

} // namespace

static Error duplicateIdError(StringRef id) {
  return createStringError(inconvertibleErrorCode(),
                           "checker id already registered: %s", id.data());
}

Error CheckerRegistry::registerDeclarative(const CheckerSpec &spec) {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.metadata.id == spec.metadata.id) {
      return duplicateIdError(spec.metadata.id);
    }
  }

  CheckerDescriptor descriptor;
  descriptor.metadata = spec.metadata;
  descriptor.rule_kind = spec.rule_kind;
  descriptor.capabilities = spec.capabilities;
  descriptor.executable = true;
  descriptor.spec = spec;
  descriptors_.push_back(std::move(descriptor));
  return Error::success();
}

Error CheckerRegistry::registerNative(
    const CheckerMetadata &metadata, RuleKind rule_kind,
    std::vector<CheckerCapability> capabilities, bool executable) {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.metadata.id == metadata.id) {
      return duplicateIdError(metadata.id);
    }
  }

  CheckerDescriptor descriptor;
  descriptor.metadata = metadata;
  descriptor.rule_kind = rule_kind;
  descriptor.capabilities = std::move(capabilities);
  descriptor.executable = executable;
  descriptors_.push_back(std::move(descriptor));
  return Error::success();
}

Expected<const CheckerDescriptor *>
CheckerRegistry::findById(StringRef id) const {
  for (const auto &descriptor : descriptors_) {
    if (descriptor.metadata.id == id) {
      return &descriptor;
    }
  }
  return createStringError(inconvertibleErrorCode(), "unknown checker id: %s",
                           id.data());
}

std::vector<const CheckerDescriptor *> CheckerRegistry::list() const {
  std::vector<const CheckerDescriptor *> result;
  result.reserve(descriptors_.size());
  for (const auto &descriptor : descriptors_) {
    result.push_back(&descriptor);
  }
  return result;
}

std::vector<const CheckerDescriptor *>
CheckerRegistry::select(StringRef category,
                        std::optional<EngineKind> engine) const {
  std::vector<const CheckerDescriptor *> result;
  for (const auto &descriptor : descriptors_) {
    if (!category.empty() && descriptor.metadata.category != category) {
      continue;
    }
    if (engine.has_value() && descriptor.metadata.engine != *engine) {
      continue;
    }
    result.push_back(&descriptor);
  }
  return result;
}

Error registerBuiltinNativeCheckers(CheckerRegistry &registry) {
  auto add_native = [&](const char *id, const char *title, const char *category,
                        Severity severity, EngineKind engine,
                        std::vector<CheckerCapability> capabilities) -> Error {
    CheckerMetadata metadata;
    metadata.id = id;
    metadata.title = title;
    metadata.category = category;
    metadata.summary = title;
    metadata.severity = severity;
    metadata.engine = engine;
    metadata.languages = {"llvm-ir"};
    metadata.default_enabled = true;
    return registry.registerNative(metadata, RuleKind::Native,
                                   std::move(capabilities), false);
  };

  if (Error error = add_native(
          "ae", "Abstract Execution", "memory-safety", Severity::High,
          EngineKind::AE, {CheckerCapability::ICFG, CheckerCapability::SMT})) {
    return error;
  }
  if (Error error =
          add_native("saber", "Saber Source-Sink", "memory-safety",
                     Severity::High, EngineKind::Saber,
                     {CheckerCapability::SVFG, CheckerCapability::PTA})) {
    return error;
  }
  if (Error error =
          add_native("pulse", "Pulse Checker", "memory-safety", Severity::High,
                     EngineKind::Pulse,
                     {CheckerCapability::UseDef, CheckerCapability::SMT})) {
    return error;
  }
  if (Error error = add_native(
          "kint", "KINT", "integer", Severity::High, EngineKind::KINT,
          {CheckerCapability::SMT, CheckerCapability::ICFG})) {
    return error;
  }
  if (Error error = add_native(
          "taint", "IFDS Taint Analysis", "security", Severity::High,
          EngineKind::Taint,
          {CheckerCapability::InterproceduralFlow, CheckerCapability::PTA})) {
    return error;
  }
  if (Error error =
          add_native("fitx", "FiTx", "api-misuse", Severity::Medium,
                     EngineKind::FiTx, {CheckerCapability::DirectCalls})) {
    return error;
  }
  if (Error error =
          add_native("concur", "Concurrency Checker", "concurrency",
                     Severity::High, EngineKind::Concurrency,
                     {CheckerCapability::MHP, CheckerCapability::PTA})) {
    return error;
  }
  if (Error error =
          add_native("symex", "Symbolic Execution", "path-sensitive",
                     Severity::High, EngineKind::SymExec,
                     {CheckerCapability::SVFG, CheckerCapability::SMT})) {
    return error;
  }
  return Error::success();
}

ArrayRef<NativeCheckDescriptor> getBuiltinNativeChecks(EngineKind engine) {
  switch (engine) {
  case EngineKind::AE:
    return AE_CHECKS;
  case EngineKind::Saber:
    return SABER_CHECKS;
  case EngineKind::Pulse:
    return PULSE_CHECKS;
  case EngineKind::KINT:
    return KINT_CHECKS;
  case EngineKind::Taint:
    return TAINT_CHECKS;
  case EngineKind::FiTx:
    return FITX_CHECKS;
  case EngineKind::Concurrency:
    return CONCURRENCY_CHECKS;
  case EngineKind::SymExec:
    return SYMEX_CHECKS;
  case EngineKind::Declarative:
    return {};
  }
  llvm_unreachable("unhandled checker engine");
}

} // namespace lotus::checker

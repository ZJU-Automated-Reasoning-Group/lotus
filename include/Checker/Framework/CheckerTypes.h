/** @file CheckerTypes.h @brief Core type definitions for the checker framework
 * (engine kinds, options). */
#pragma once

#include "Checker/Framework/BugTypes.h"

#include <string>
#include <vector>

namespace lotus::checker {

enum class EngineKind {
  Declarative,
  AE,
  Saber,
  Pulse,
  KINT,
  Taint,
  FiTx,
  Concurrency,
  SymExec
};

enum class CheckerCapability {
  DebugInfo,
  DirectCalls,
  UseDef,
  SimpleMemory,
  InterproceduralFlow,
  ICFG,
  SVFG,
  PTA,
  MHP,
  SMT
};

enum class Severity { Low, Medium, High, Critical };

enum class RuleKind { ForbiddenCall, SourceSink, ApiProtocol, Native };

struct CheckerMetadata {
  std::string id;
  std::string title;
  std::string category;
  std::string summary;
  Severity severity = Severity::Medium;
  EngineKind engine = EngineKind::Declarative;
  std::vector<std::string> languages;
  std::vector<std::string> tags;
  bool default_enabled = true;
};

struct ForbiddenCallRule {
  std::vector<std::string> functions;
};

enum class DataSelectorKind {
  Return,
  Argument,
  ArgumentMemory,
  AnyArgument,
};

struct DataEndpoint {
  std::string function;
  DataSelectorKind selector_kind = DataSelectorKind::Return;
  unsigned selector_arg = 0;
};

struct SourceSinkRule {
  // Legacy name-only models remain supported for existing checker specs.
  std::vector<std::string> sources;
  std::vector<std::string> sinks;
  std::vector<std::string> sanitizers;
  std::vector<DataEndpoint> source_models;
  std::vector<DataEndpoint> sink_models;
  std::vector<DataEndpoint> sanitizer_models;
};

enum class ResourceSelectorKind { Return, Argument };

struct ProtocolOperation {
  std::string function;
  ResourceSelectorKind resource_kind = ResourceSelectorKind::Argument;
  unsigned resource_arg = 0;
};

struct ApiProtocolRule {
  std::vector<ProtocolOperation> acquire;
  std::vector<ProtocolOperation> use;
  std::vector<ProtocolOperation> release;
  bool report_leak = true;
  bool report_use_before_acquire = true;
  bool report_use_after_release = true;
  bool report_double_acquire = true;
  bool report_double_release = true;
};

struct CheckerSpec {
  CheckerMetadata metadata;
  RuleKind rule_kind = RuleKind::Native;
  std::vector<CheckerCapability> capabilities;
  std::string message;
  std::string suggestion;
  int confidence = 80;

  ForbiddenCallRule forbidden_call;
  SourceSinkRule source_sink;
  ApiProtocolRule api_protocol;

  bool isDeclarative() const {
    return metadata.engine == EngineKind::Declarative;
  }
};

const char *toString(EngineKind kind);
const char *toString(CheckerCapability capability);
const char *toString(Severity severity);
const char *toString(RuleKind kind);

BugDescription::BugImportance severityToImportance(Severity severity);
Severity importanceToSeverity(BugDescription::BugImportance importance);

} // namespace lotus::checker

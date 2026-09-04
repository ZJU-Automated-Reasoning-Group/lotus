#include "Checker/Framework/CheckerTypes.h"

namespace lotus::checker {

const char *toString(EngineKind kind) {
  switch (kind) {
  case EngineKind::Declarative:
    return "declarative";
  case EngineKind::AE:
    return "ae";
  case EngineKind::Saber:
    return "saber";
  case EngineKind::Pulse:
    return "pulse";
  case EngineKind::KINT:
    return "kint";
  case EngineKind::Taint:
    return "taint";
  case EngineKind::FiTx:
    return "fitx";
  case EngineKind::Concurrency:
    return "concur";
  case EngineKind::SymExec:
    return "symex";
  }
  return "unknown";
}

const char *toString(CheckerCapability capability) {
  switch (capability) {
  case CheckerCapability::DebugInfo:
    return "debug-info";
  case CheckerCapability::DirectCalls:
    return "direct-calls";
  case CheckerCapability::UseDef:
    return "use-def";
  case CheckerCapability::SimpleMemory:
    return "simple-memory";
  case CheckerCapability::InterproceduralFlow:
    return "interprocedural-flow";
  case CheckerCapability::ICFG:
    return "icfg";
  case CheckerCapability::SVFG:
    return "svfg";
  case CheckerCapability::PTA:
    return "pta";
  case CheckerCapability::MHP:
    return "mhp";
  case CheckerCapability::SMT:
    return "smt";
  }
  return "unknown";
}

const char *toString(Severity severity) {
  switch (severity) {
  case Severity::Low:
    return "low";
  case Severity::Medium:
    return "medium";
  case Severity::High:
    return "high";
  case Severity::Critical:
    return "critical";
  }
  return "unknown";
}

const char *toString(RuleKind kind) {
  switch (kind) {
  case RuleKind::ForbiddenCall:
    return "forbidden_call";
  case RuleKind::SourceSink:
    return "source_sink";
  case RuleKind::ApiProtocol:
    return "api_protocol";
  case RuleKind::Native:
    return "native";
  }
  return "unknown";
}

BugDescription::BugImportance severityToImportance(Severity severity) {
  switch (severity) {
  case Severity::Low:
    return BugDescription::BI_LOW;
  case Severity::Medium:
    return BugDescription::BI_MEDIUM;
  case Severity::High:
  case Severity::Critical:
    return BugDescription::BI_HIGH;
  }
  return BugDescription::BI_NA;
}

Severity importanceToSeverity(BugDescription::BugImportance importance) {
  switch (importance) {
  case BugDescription::BI_LOW:
    return Severity::Low;
  case BugDescription::BI_MEDIUM:
    return Severity::Medium;
  case BugDescription::BI_HIGH:
    return Severity::High;
  case BugDescription::BI_NA:
    return Severity::Low;
  }
  return Severity::Medium;
}

} // namespace lotus::checker

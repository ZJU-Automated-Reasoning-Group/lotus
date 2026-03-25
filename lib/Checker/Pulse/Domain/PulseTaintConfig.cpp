#include "Checker/Pulse/Domain/PulseTaintConfig.h"

#include <algorithm>

#include <llvm/IR/Function.h>

namespace pulse {

bool ProcedureMatcher::matches(const llvm::Function *func) const {
  if (!func)
    return false;

  std::string func_name = func->getName().str();

  switch (type_) {
  case Type::ProcedureName:
    return func_name == name_;

  case Type::ProcedureNameRegex:
    return std::regex_search(func_name, name_regex_);

  case Type::BuiltinName:
    return func->isIntrinsic() && func_name == name_;

  case Type::ClassNameRegex:
  case Type::ClassAndMethodNames:
    // Simplified: check if function name contains class name
    // Full implementation would parse class name from function signature
    for (const auto &class_name : class_names_) {
      if (func_name.find(class_name) != std::string::npos) {
        return true;
      }
    }
    return false;

  default:
    return false;
  }
}

std::vector<TaintKind> TaintConfig::isSource(const llvm::Function *func) const {
  std::vector<TaintKind> kinds;

  for (const auto &unit : source_procedures_) {
    if (unit.matcher.matches(func)) {
      kinds.insert(kinds.end(), unit.kinds.begin(), unit.kinds.end());
    }
  }

  return kinds;
}

bool TaintConfig::isSink(const llvm::Function *func,
                         std::vector<TaintKind> &sink_kinds) const {
  sink_kinds.clear();

  for (const auto &unit : sink_procedures_) {
    if (unit.matcher.matches(func)) {
      sink_kinds.insert(sink_kinds.end(), unit.kinds.begin(), unit.kinds.end());
      return true;
    }
  }

  return false;
}

std::vector<TaintKind>
TaintConfig::isSanitizer(const llvm::Function *func) const {
  std::vector<TaintKind> kinds;

  for (const auto &unit : sanitizer_procedures_) {
    if (unit.matcher.matches(func)) {
      kinds.insert(kinds.end(), unit.kinds.begin(), unit.kinds.end());
    }
  }

  return kinds;
}

bool TaintConfig::isPropagator(const llvm::Function *func) const {
  for (const auto &unit : propagator_procedures_) {
    if (unit.matcher.matches(func)) {
      return true;
    }
  }
  return false;
}

const std::vector<SinkPolicy> &
TaintConfig::getSinkPolicies(const TaintKind &sink_kind) const {
  auto it = sink_policies_.find(sink_kind);
  if (it != sink_policies_.end()) {
    return it->second;
  }
  static const std::vector<SinkPolicy> empty;
  return empty;
}

std::vector<TaintKind>
TaintConfig::getAllocationSourceKinds(const std::string &class_name) const {
  auto it = allocation_sources_.find(class_name);
  if (it != allocation_sources_.end()) {
    return it->second;
  }
  return {};
}

TaintConfig *TaintConfig::default_config_ = nullptr;

void TaintConfig::initializeDefault() {
  if (default_config_)
    return;

  default_config_ = new TaintConfig();

  // Common taint sources
  ProcedureMatcher read_matcher(ProcedureMatcher::Type::ProcedureNameRegex,
                                "read|fread|recv");
  ProcedureUnit read_source(read_matcher, {TaintKind::UserInput()},
                            ProcedureTarget::ReturnValue);
  default_config_->addSourceProcedure(read_source);

  ProcedureMatcher getenv_matcher(ProcedureMatcher::Type::ProcedureName,
                                  "getenv");
  ProcedureUnit getenv_source(getenv_matcher, {TaintKind::Environment()},
                              ProcedureTarget::ReturnValue);
  default_config_->addSourceProcedure(getenv_source);

  // Common taint sinks
  ProcedureMatcher system_matcher(ProcedureMatcher::Type::ProcedureName,
                                  "system");
  ProcedureUnit system_sink(system_matcher, {TaintKind::UserInput()},
                            ProcedureTarget::AllArguments);
  default_config_->addSinkProcedure(system_sink);

  ProcedureMatcher exec_matcher(ProcedureMatcher::Type::ProcedureNameRegex,
                                "exec|popen");
  ProcedureUnit exec_sink(exec_matcher, {TaintKind::UserInput()},
                          ProcedureTarget::AllArguments);
  default_config_->addSinkProcedure(exec_sink);

  // Common sanitizers
  ProcedureMatcher sanitizer_matcher(ProcedureMatcher::Type::ProcedureNameRegex,
                                     "strlen|atoi|strtol");
  ProcedureUnit sanitizer_unit(sanitizer_matcher, {TaintKind::Unknown()},
                               ProcedureTarget::AllArguments);
  default_config_->addSanitizerProcedure(sanitizer_unit);

  // Sink policies
  SinkPolicy command_injection(1, "Command injection");
  command_injection.source_kinds = {TaintKind::UserInput(),
                                    TaintKind::Network()};
  command_injection.sanitizer_kinds = {};
  default_config_->addSinkPolicy(TaintKind::UserInput(), command_injection);
}

const TaintConfig &TaintConfig::getDefault() {
  if (!default_config_) {
    initializeDefault();
  }
  return *default_config_;
}

} // namespace pulse

#include "Checker/Framework/CheckerValidator.h"

#include <llvm/Support/Error.h>

using namespace llvm;

namespace lotus::checker {

static Error validationError(const CheckerSpec &spec, const char *message) {
  return createStringError(inconvertibleErrorCode(),
                           "invalid checker spec '%s': %s",
                           spec.metadata.id.c_str(), message);
}

Error CheckerValidator::validate(
    const CheckerSpec &spec,
    const std::unordered_set<std::string> &existing_ids) {
  if (spec.metadata.id.empty()) {
    return createStringError(inconvertibleErrorCode(),
                             "invalid checker spec: missing metadata.id");
  }
  if (existing_ids.count(spec.metadata.id)) {
    return createStringError(inconvertibleErrorCode(),
                             "invalid checker spec '%s': duplicate id",
                             spec.metadata.id.c_str());
  }
  if (spec.metadata.title.empty()) {
    return validationError(spec, "missing metadata.title");
  }
  if (!spec.isDeclarative()) {
    return validationError(spec,
                           "declarative specs must use declarative engine");
  }
  if (spec.message.empty()) {
    return validationError(spec, "missing message");
  }
  if (spec.confidence < 0 || spec.confidence > 100) {
    return validationError(spec, "confidence must be in [0,100]");
  }

  switch (spec.rule_kind) {
  case RuleKind::ForbiddenCall:
    if (spec.forbidden_call.functions.empty()) {
      return validationError(spec, "forbidden_call requires functions");
    }
    break;
  case RuleKind::SourceSink:
    if ((spec.source_sink.sources.empty() &&
         spec.source_sink.source_models.empty()) ||
        (spec.source_sink.sinks.empty() &&
         spec.source_sink.sink_models.empty())) {
      return validationError(spec, "source_sink requires sources and sinks");
    }
    break;
  case RuleKind::ApiProtocol:
    if (spec.api_protocol.acquire.empty() || spec.api_protocol.use.empty() ||
        spec.api_protocol.release.empty()) {
      return validationError(spec, "api_protocol requires acquire/use/release");
    }
    break;
  case RuleKind::Native:
    return validationError(spec,
                           "native rule kind is not allowed in YAML specs");
  }

  return Error::success();
}

} // namespace lotus::checker

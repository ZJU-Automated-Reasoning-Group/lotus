#include "Checker/Framework/CheckerSpecLoader.h"

#include "Checker/Framework/CheckerValidator.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_set>

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/YAMLTraits.h>

using namespace llvm;

namespace fs = std::filesystem;

namespace lotus::checker {

struct YamlMetadata {
  std::string id;
  std::string title;
  std::string category;
  std::string summary;
  std::string severity;
  std::vector<std::string> languages;
  std::vector<std::string> tags;
  bool default_enabled = true;
};

struct YamlProtocolOperation {
  std::string function;
  std::string resource;
  int resource_arg = -1;
};

struct YamlDataEndpoint {
  std::string function;
  std::string selector;
  int arg = -1;
};

struct YamlSpec {
  std::string engine;
  std::string rule_kind;
  YamlMetadata metadata;
  std::vector<std::string> capabilities;
  std::string message;
  std::string suggestion;
  int confidence = 80;
  std::vector<std::string> functions;
  std::vector<std::string> sources;
  std::vector<std::string> sinks;
  std::vector<std::string> sanitizers;
  std::vector<YamlDataEndpoint> source_models;
  std::vector<YamlDataEndpoint> sink_models;
  std::vector<YamlDataEndpoint> sanitizer_models;
  std::vector<YamlProtocolOperation> acquire;
  std::vector<YamlProtocolOperation> use;
  std::vector<YamlProtocolOperation> release;
  bool report_leak = true;
  bool report_use_before_acquire = true;
  bool report_use_after_release = true;
  bool report_double_acquire = true;
  bool report_double_release = true;
};

namespace {

Expected<EngineKind> parseEngineKind(StringRef value) {
  if (value == "declarative") {
    return EngineKind::Declarative;
  }
  if (value == "ae") {
    return EngineKind::AE;
  }
  if (value == "saber") {
    return EngineKind::Saber;
  }
  if (value == "pulse") {
    return EngineKind::Pulse;
  }
  if (value == "kint") {
    return EngineKind::KINT;
  }
  if (value == "taint") {
    return EngineKind::Taint;
  }
  if (value == "fitx") {
    return EngineKind::FiTx;
  }
  if (value == "concurrency") {
    return EngineKind::Concurrency;
  }
  if (value == "symex") {
    return EngineKind::SymExec;
  }
  return createStringError(inconvertibleErrorCode(), "unknown engine '%s'",
                           value.str().c_str());
}

Expected<Severity> parseSeverity(StringRef value) {
  if (value == "low") {
    return Severity::Low;
  }
  if (value == "high") {
    return Severity::High;
  }
  if (value == "critical") {
    return Severity::Critical;
  }
  if (value == "medium") {
    return Severity::Medium;
  }
  return createStringError(inconvertibleErrorCode(), "unknown severity '%s'",
                           value.str().c_str());
}

Expected<RuleKind> parseRuleKind(StringRef value) {
  if (value == "forbidden_call") {
    return RuleKind::ForbiddenCall;
  }
  if (value == "source_sink") {
    return RuleKind::SourceSink;
  }
  if (value == "api_protocol") {
    return RuleKind::ApiProtocol;
  }
  return createStringError(inconvertibleErrorCode(), "unknown rule kind '%s'",
                           value.str().c_str());
}

Expected<std::vector<ProtocolOperation>>
parseProtocolOperations(const std::vector<YamlProtocolOperation> &operations,
                        StringRef operationKind, StringRef sourceName) {
  std::vector<ProtocolOperation> result;
  result.reserve(operations.size());
  for (const YamlProtocolOperation &operation : operations) {
    if (operation.function.empty()) {
      return createStringError(
          inconvertibleErrorCode(), "%s: %s operation requires function",
          sourceName.str().c_str(), operationKind.str().c_str());
    }
    const bool selectsReturn = operation.resource == "return";
    const bool selectsArgument = operation.resource_arg >= 0;
    if (selectsReturn == selectsArgument) {
      return createStringError(
          inconvertibleErrorCode(),
          "%s: %s operation '%s' must specify exactly one of resource: return "
          "or resource_arg",
          sourceName.str().c_str(), operationKind.str().c_str(),
          operation.function.c_str());
    }

    ProtocolOperation parsed;
    parsed.function = operation.function;
    parsed.resource_kind = selectsReturn ? ResourceSelectorKind::Return
                                         : ResourceSelectorKind::Argument;
    parsed.resource_arg =
        selectsArgument ? static_cast<unsigned>(operation.resource_arg) : 0;
    result.push_back(std::move(parsed));
  }
  return result;
}

Expected<std::vector<DataEndpoint>>
parseDataEndpoints(const std::vector<YamlDataEndpoint> &endpoints,
                   StringRef endpointKind, StringRef sourceName) {
  std::vector<DataEndpoint> result;
  result.reserve(endpoints.size());
  for (const YamlDataEndpoint &endpoint : endpoints) {
    if (endpoint.function.empty()) {
      return createStringError(
          inconvertibleErrorCode(), "%s: %s model requires function",
          sourceName.str().c_str(), endpointKind.str().c_str());
    }

    DataEndpoint parsed;
    parsed.function = endpoint.function;
    if (endpoint.selector == "return") {
      if (endpoint.arg >= 0) {
        return createStringError(inconvertibleErrorCode(),
                                 "%s: return selector must not specify arg",
                                 sourceName.str().c_str());
      }
      parsed.selector_kind = DataSelectorKind::Return;
    } else if (endpoint.selector == "argument" || endpoint.selector == "arg") {
      if (endpoint.arg < 0) {
        return createStringError(inconvertibleErrorCode(),
                                 "%s: argument selector requires arg",
                                 sourceName.str().c_str());
      }
      parsed.selector_kind = DataSelectorKind::Argument;
      parsed.selector_arg = static_cast<unsigned>(endpoint.arg);
    } else if (endpoint.selector == "memory") {
      if (endpoint.arg < 0) {
        return createStringError(inconvertibleErrorCode(),
                                 "%s: memory selector requires arg",
                                 sourceName.str().c_str());
      }
      parsed.selector_kind = DataSelectorKind::ArgumentMemory;
      parsed.selector_arg = static_cast<unsigned>(endpoint.arg);
    } else if (endpoint.selector == "any-argument" && endpointKind == "sink") {
      if (endpoint.arg >= 0) {
        return createStringError(
            inconvertibleErrorCode(),
            "%s: any-argument selector must not specify arg",
            sourceName.str().c_str());
      }
      parsed.selector_kind = DataSelectorKind::AnyArgument;
    } else {
      return createStringError(
          inconvertibleErrorCode(), "%s: unknown %s selector '%s'",
          sourceName.str().c_str(), endpointKind.str().c_str(),
          endpoint.selector.c_str());
    }
    result.push_back(std::move(parsed));
  }
  return result;
}

std::optional<CheckerCapability> parseCapability(StringRef value) {
  if (value == "debug-info") {
    return CheckerCapability::DebugInfo;
  }
  if (value == "direct-calls") {
    return CheckerCapability::DirectCalls;
  }
  if (value == "use-def") {
    return CheckerCapability::UseDef;
  }
  if (value == "simple-memory") {
    return CheckerCapability::SimpleMemory;
  }
  if (value == "interprocedural-flow") {
    return CheckerCapability::InterproceduralFlow;
  }
  if (value == "icfg") {
    return CheckerCapability::ICFG;
  }
  if (value == "svfg") {
    return CheckerCapability::SVFG;
  }
  if (value == "pta") {
    return CheckerCapability::PTA;
  }
  if (value == "mhp") {
    return CheckerCapability::MHP;
  }
  if (value == "smt") {
    return CheckerCapability::SMT;
  }
  return std::nullopt;
}

} // namespace
} // namespace lotus::checker

LLVM_YAML_IS_SEQUENCE_VECTOR(lotus::checker::YamlProtocolOperation)
LLVM_YAML_IS_SEQUENCE_VECTOR(lotus::checker::YamlDataEndpoint)

namespace llvm::yaml {

template <> struct MappingTraits<lotus::checker::YamlDataEndpoint> {
  static void mapping(IO &io, lotus::checker::YamlDataEndpoint &endpoint) {
    io.mapRequired("function", endpoint.function);
    io.mapRequired("selector", endpoint.selector);
    io.mapOptional("arg", endpoint.arg, -1);
  }
};

template <> struct MappingTraits<lotus::checker::YamlProtocolOperation> {
  static void mapping(IO &io,
                      lotus::checker::YamlProtocolOperation &operation) {
    io.mapRequired("function", operation.function);
    io.mapOptional("resource", operation.resource, std::string());
    io.mapOptional("resource_arg", operation.resource_arg, -1);
  }
};

template <> struct MappingTraits<lotus::checker::YamlMetadata> {
  static void mapping(IO &io, lotus::checker::YamlMetadata &metadata) {
    io.mapRequired("id", metadata.id);
    io.mapRequired("title", metadata.title);
    io.mapRequired("category", metadata.category);
    io.mapOptional("summary", metadata.summary);
    io.mapOptional("severity", metadata.severity, std::string("medium"));
    io.mapOptional("languages", metadata.languages);
    io.mapOptional("tags", metadata.tags);
    io.mapOptional("default_enabled", metadata.default_enabled, true);
  }
};

template <> struct MappingTraits<lotus::checker::YamlSpec> {
  static void mapping(IO &io, lotus::checker::YamlSpec &spec) {
    io.mapRequired("engine", spec.engine);
    io.mapRequired("rule_kind", spec.rule_kind);
    io.mapRequired("metadata", spec.metadata);
    io.mapOptional("capabilities", spec.capabilities);
    io.mapRequired("message", spec.message);
    io.mapOptional("suggestion", spec.suggestion);
    io.mapOptional("confidence", spec.confidence, 80);
    io.mapOptional("functions", spec.functions);
    io.mapOptional("sources", spec.sources);
    io.mapOptional("sinks", spec.sinks);
    io.mapOptional("sanitizers", spec.sanitizers);
    io.mapOptional("source_models", spec.source_models);
    io.mapOptional("sink_models", spec.sink_models);
    io.mapOptional("sanitizer_models", spec.sanitizer_models);
    io.mapOptional("acquire", spec.acquire);
    io.mapOptional("use", spec.use);
    io.mapOptional("release", spec.release);
    io.mapOptional("report_leak", spec.report_leak, true);
    io.mapOptional("report_use_before_acquire", spec.report_use_before_acquire,
                   true);
    io.mapOptional("report_use_after_release", spec.report_use_after_release,
                   true);
    io.mapOptional("report_double_acquire", spec.report_double_acquire, true);
    io.mapOptional("report_double_release", spec.report_double_release, true);
  }
};

} // namespace llvm::yaml

namespace lotus::checker {

Expected<CheckerSpec>
CheckerSpecLoader::loadFromBuffer(StringRef yaml, StringRef source_name) const {
  yaml::Input input(yaml);
  YamlSpec parsed;
  input >> parsed;
  if (std::error_code error = input.error()) {
    return createStringError(error, "failed to parse YAML spec %s",
                             source_name.data());
  }

  CheckerSpec spec;
  spec.metadata.id = parsed.metadata.id;
  spec.metadata.title = parsed.metadata.title;
  spec.metadata.category = parsed.metadata.category;
  spec.metadata.summary = parsed.metadata.summary;
  auto severity = parseSeverity(parsed.metadata.severity);
  if (!severity) {
    return severity.takeError();
  }
  spec.metadata.severity = *severity;
  auto engine = parseEngineKind(parsed.engine);
  if (!engine) {
    return engine.takeError();
  }
  spec.metadata.engine = *engine;
  spec.metadata.languages = parsed.metadata.languages;
  spec.metadata.tags = parsed.metadata.tags;
  spec.metadata.default_enabled = parsed.metadata.default_enabled;
  auto rule_kind = parseRuleKind(parsed.rule_kind);
  if (!rule_kind) {
    return rule_kind.takeError();
  }
  spec.rule_kind = *rule_kind;
  spec.message = parsed.message;
  spec.suggestion = parsed.suggestion;
  spec.confidence = parsed.confidence;
  spec.forbidden_call.functions = parsed.functions;
  spec.source_sink.sources = parsed.sources;
  spec.source_sink.sinks = parsed.sinks;
  spec.source_sink.sanitizers = parsed.sanitizers;
  auto source_models =
      parseDataEndpoints(parsed.source_models, "source", source_name);
  if (!source_models) {
    return source_models.takeError();
  }
  spec.source_sink.source_models = std::move(*source_models);
  auto sink_models =
      parseDataEndpoints(parsed.sink_models, "sink", source_name);
  if (!sink_models) {
    return sink_models.takeError();
  }
  spec.source_sink.sink_models = std::move(*sink_models);
  auto sanitizer_models =
      parseDataEndpoints(parsed.sanitizer_models, "sanitizer", source_name);
  if (!sanitizer_models) {
    return sanitizer_models.takeError();
  }
  spec.source_sink.sanitizer_models = std::move(*sanitizer_models);
  auto acquire =
      parseProtocolOperations(parsed.acquire, "acquire", source_name);
  if (!acquire) {
    return acquire.takeError();
  }
  spec.api_protocol.acquire = std::move(*acquire);
  auto use = parseProtocolOperations(parsed.use, "use", source_name);
  if (!use) {
    return use.takeError();
  }
  spec.api_protocol.use = std::move(*use);
  auto release =
      parseProtocolOperations(parsed.release, "release", source_name);
  if (!release) {
    return release.takeError();
  }
  spec.api_protocol.release = std::move(*release);
  spec.api_protocol.report_leak = parsed.report_leak;
  spec.api_protocol.report_use_before_acquire =
      parsed.report_use_before_acquire;
  spec.api_protocol.report_use_after_release = parsed.report_use_after_release;
  spec.api_protocol.report_double_acquire = parsed.report_double_acquire;
  spec.api_protocol.report_double_release = parsed.report_double_release;

  for (const auto &capability_name : parsed.capabilities) {
    auto capability = parseCapability(capability_name);
    if (!capability.has_value()) {
      return createStringError(inconvertibleErrorCode(),
                               "unknown capability '%s' in spec %s",
                               capability_name.c_str(), source_name.data());
    }
    spec.capabilities.push_back(*capability);
  }

  if (Error error = CheckerValidator::validate(spec)) {
    return std::move(error);
  }

  return spec;
}

Expected<std::vector<CheckerSpec>>
CheckerSpecLoader::loadFromDirectory(StringRef directory) const {
  std::vector<CheckerSpec> specs;
  std::unordered_set<std::string> ids;
  std::error_code fs_error;
  fs::path root(directory.str());

  if (!fs::exists(root, fs_error) || !fs::is_directory(root, fs_error)) {
    return createStringError(inconvertibleErrorCode(),
                             "spec directory does not exist: %s",
                             directory.data());
  }

  std::vector<fs::path> paths;
  for (const auto &entry : fs::recursive_directory_iterator(root, fs_error)) {
    if (fs_error) {
      return createStringError(fs_error, "failed while walking spec directory");
    }
    if (!entry.is_regular_file() || entry.path().extension() != ".yml") {
      continue;
    }

    paths.push_back(entry.path());
  }

  std::sort(paths.begin(), paths.end(),
            [](const fs::path &left, const fs::path &right) {
              return left.generic_string() < right.generic_string();
            });

  for (const fs::path &path : paths) {
    auto buffer = MemoryBuffer::getFile(path.string());
    if (!buffer) {
      return createStringError(buffer.getError(), "failed to open spec file");
    }

    auto spec_or = loadFromBuffer(buffer.get()->getBuffer(), path.string());
    if (!spec_or) {
      return spec_or.takeError();
    }
    if (Error error = CheckerValidator::validate(*spec_or, ids)) {
      return std::move(error);
    }
    ids.insert(spec_or->metadata.id);
    specs.push_back(std::move(*spec_or));
  }

  return specs;
}

} // namespace lotus::checker

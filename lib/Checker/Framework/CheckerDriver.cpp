#include "Checker/Framework/CheckerDriver.h"

#include "Checker/Framework/BugReportMgr.h"

#include <algorithm>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <tuple>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Error.h>

using namespace llvm;

namespace lotus::checker {
namespace {

static std::string calleeName(const CallBase &call) {
  const Value *called = call.getCalledOperand();
  if (!called) {
    return {};
  }
  if (const auto *callee = dyn_cast<Function>(called->stripPointerCasts())) {
    return callee->getName().str();
  }
  return {};
}

static const Function *directCallee(const CallBase &call) {
  const Value *called = call.getCalledOperand();
  return called ? dyn_cast<Function>(called->stripPointerCasts()) : nullptr;
}

static const Value *canonicalPointer(const Value *value) {
  SmallPtrSet<const Value *, 8> visited;
  while (value && visited.insert(value).second) {
    const Value *stripped = value->stripPointerCasts();
    if (stripped != value) {
      value = stripped;
      continue;
    }
    if (const auto *gep = dyn_cast<GEPOperator>(value)) {
      value = gep->getPointerOperand();
      continue;
    }
    break;
  }
  return value;
}

static bool anyNameMatches(const std::string &name,
                           const std::vector<std::string> &patterns) {
  return std::find(patterns.begin(), patterns.end(), name) != patterns.end();
}

static std::vector<CheckerDiagnostic>
runForbiddenCall(const CheckerSpec &spec, CheckerContext &context) {
  std::vector<CheckerDiagnostic> diagnostics;
  for (auto &function : context.module) {
    for (auto &instruction : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (!call) {
        continue;
      }
      const std::string name = calleeName(*call);
      if (name.empty() ||
          !anyNameMatches(name, spec.forbidden_call.functions)) {
        continue;
      }

      CheckerDiagnostic diagnostic;
      diagnostic.checker_id = spec.metadata.id;
      diagnostic.bug_type = spec.metadata.title;
      diagnostic.severity = spec.metadata.severity;
      diagnostic.primary_value = call;
      diagnostic.message = spec.message;
      diagnostic.suggestion = spec.suggestion;
      diagnostic.confidence = spec.confidence;
      diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
      diagnostic.metadata["callee"] = name;
      diagnostics.push_back(std::move(diagnostic));
    }
  }
  return diagnostics;
}

static std::vector<CheckerDiagnostic> runSourceSink(const CheckerSpec &spec,
                                                    CheckerContext &context) {
  std::vector<CheckerDiagnostic> diagnostics;
  SmallPtrSet<const Value *, 32> tainted_values;
  SmallPtrSet<const Value *, 16> sanitized_values;
  SmallPtrSet<const Value *, 16> tainted_memory;
  SmallPtrSet<const Value *, 16> sanitized_memory;
  std::queue<const Value *> value_worklist;
  std::queue<const Value *> memory_worklist;
  std::map<const Value *, const Value *> value_origins;
  std::map<const Value *, const Value *> memory_origins;

  auto enqueue_tainted = [&](const Value *value, const Value *origin) {
    if (value && tainted_values.insert(value).second) {
      value_origins[value] = origin ? origin : value;
      value_worklist.push(value);
    }
  };
  auto enqueue_memory = [&](const Value *pointer, const Value *origin) {
    pointer = canonicalPointer(pointer);
    if (pointer && tainted_memory.insert(pointer).second) {
      memory_origins[pointer] = origin ? origin : pointer;
      memory_worklist.push(pointer);
    }
  };

  std::vector<DataEndpoint> sources = spec.source_sink.source_models;
  std::vector<DataEndpoint> sinks = spec.source_sink.sink_models;
  std::vector<DataEndpoint> sanitizers = spec.source_sink.sanitizer_models;
  for (const std::string &name : spec.source_sink.sources) {
    sources.push_back({name, DataSelectorKind::Return, 0});
  }
  for (const std::string &name : spec.source_sink.sinks) {
    sinks.push_back({name, DataSelectorKind::AnyArgument, 0});
  }
  for (const std::string &name : spec.source_sink.sanitizers) {
    sanitizers.push_back({name, DataSelectorKind::Return, 0});
  }

  auto matchingEndpoints = [](StringRef name,
                              const std::vector<DataEndpoint> &endpoints) {
    std::vector<const DataEndpoint *> matches;
    for (const DataEndpoint &endpoint : endpoints) {
      if (endpoint.function == name) {
        matches.push_back(&endpoint);
      }
    }
    return matches;
  };

  auto selectedArgument = [](const CallBase &call,
                             const DataEndpoint &endpoint) -> const Value * {
    return endpoint.selector_arg < call.arg_size()
               ? call.getArgOperand(endpoint.selector_arg)
               : nullptr;
  };

  for (auto &function : context.module) {
    for (auto &instruction : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (!call) {
        continue;
      }
      const std::string name = calleeName(*call);
      if (name.empty()) {
        continue;
      }
      for (const DataEndpoint *source : matchingEndpoints(name, sources)) {
        switch (source->selector_kind) {
        case DataSelectorKind::Return:
          if (!call->getType()->isVoidTy()) {
            enqueue_tainted(call, call);
          }
          break;
        case DataSelectorKind::Argument:
          enqueue_tainted(selectedArgument(*call, *source), call);
          break;
        case DataSelectorKind::ArgumentMemory:
          enqueue_memory(selectedArgument(*call, *source), call);
          break;
        case DataSelectorKind::AnyArgument:
          for (const Use &arg : call->args()) {
            enqueue_tainted(arg.get(), call);
          }
          break;
        }
      }
    }
  }

  while (!value_worklist.empty() || !memory_worklist.empty()) {
    if (!memory_worklist.empty()) {
      const Value *pointer = memory_worklist.front();
      memory_worklist.pop();

      const Value *origin = memory_origins[pointer];
      for (Function &function : context.module) {
        for (Instruction &instruction : instructions(function)) {
          if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
            if (canonicalPointer(load->getPointerOperand()) == pointer) {
              enqueue_tainted(load, origin);
            }
            continue;
          }
          const auto *call = dyn_cast<CallBase>(&instruction);
          if (!call) {
            continue;
          }
          const std::string name = calleeName(*call);
          bool isMemorySanitizer = false;
          for (const DataEndpoint *sanitizer :
               matchingEndpoints(name, sanitizers)) {
            if (sanitizer->selector_kind != DataSelectorKind::ArgumentMemory ||
                sanitizer->selector_arg >= call->arg_size()) {
              continue;
            }
            if (canonicalPointer(
                    call->getArgOperand(sanitizer->selector_arg)) == pointer) {
              sanitized_memory.insert(pointer);
              isMemorySanitizer = true;
            }
          }
          if (isMemorySanitizer) {
            continue;
          }
          const Function *callee = directCallee(*call);
          if (!callee || callee->isDeclaration()) {
            continue;
          }
          for (unsigned index = 0;
               index < call->arg_size() && index < callee->arg_size();
               ++index) {
            if (canonicalPointer(call->getArgOperand(index)) == pointer) {
              enqueue_memory(callee->getArg(index), origin);
            }
          }
        }
      }

      if (const auto *argument = dyn_cast<Argument>(pointer)) {
        const Function *function = argument->getParent();
        const unsigned index = argument->getArgNo();
        for (const User *user : function->users()) {
          if (const auto *call = dyn_cast<CallBase>(user)) {
            if (directCallee(*call) == function && index < call->arg_size()) {
              enqueue_memory(call->getArgOperand(index), origin);
            }
          }
        }
      }
      continue;
    }

    const Value *value = value_worklist.front();
    value_worklist.pop();
    const Value *origin = value_origins[value];

    for (const auto *user : value->users()) {
      if (const auto *call = dyn_cast<CallBase>(user)) {
        const std::string name = calleeName(*call);
        bool isSanitizer = false;
        for (const DataEndpoint *sanitizer :
             matchingEndpoints(name, sanitizers)) {
          isSanitizer = true;
          switch (sanitizer->selector_kind) {
          case DataSelectorKind::Return:
            sanitized_values.insert(call);
            break;
          case DataSelectorKind::Argument:
            if (const Value *argument = selectedArgument(*call, *sanitizer)) {
              sanitized_values.insert(argument);
            }
            break;
          case DataSelectorKind::ArgumentMemory:
            if (const Value *argument = selectedArgument(*call, *sanitizer)) {
              sanitized_memory.insert(canonicalPointer(argument));
            }
            break;
          case DataSelectorKind::AnyArgument:
            break;
          }
        }
        if (isSanitizer) {
          continue;
        }

        if (const Function *callee = directCallee(*call);
            callee && !callee->isDeclaration()) {
          for (unsigned index = 0;
               index < call->arg_size() && index < callee->arg_size();
               ++index) {
            if (call->getArgOperand(index) == value) {
              enqueue_tainted(callee->getArg(index), origin);
            }
          }
        }
        continue;
      }

      if (const auto *store = dyn_cast<StoreInst>(user)) {
        if (store->getValueOperand() == value) {
          enqueue_memory(store->getPointerOperand(), origin);
        }
        continue;
      }
      if (const auto *ret = dyn_cast<ReturnInst>(user)) {
        const Function *function = ret->getFunction();
        for (const User *functionUser : function->users()) {
          if (const auto *call = dyn_cast<CallBase>(functionUser)) {
            if (directCallee(*call) == function &&
                !call->getType()->isVoidTy()) {
              enqueue_tainted(call, origin);
            }
          }
        }
        continue;
      }
      if (const auto *instruction = dyn_cast<Instruction>(user)) {
        if (!isa<LoadInst>(instruction) &&
            !instruction->getType()->isVoidTy()) {
          enqueue_tainted(instruction, origin);
        }
      }
    }
  }

  std::set<const CallBase *> emitted;
  for (Function &function : context.module) {
    for (Instruction &instruction : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&instruction);
      if (!call) {
        continue;
      }
      const std::string name = calleeName(*call);
      for (const DataEndpoint *sink : matchingEndpoints(name, sinks)) {
        const Value *origin = nullptr;
        auto valueIsTainted = [&](const Value *candidate) {
          if (!candidate || sanitized_values.contains(candidate) ||
              !tainted_values.contains(candidate)) {
            return false;
          }
          origin = value_origins[candidate];
          return true;
        };
        auto memoryIsTainted = [&](const Value *candidate) {
          candidate = canonicalPointer(candidate);
          if (!candidate || sanitized_memory.contains(candidate) ||
              !tainted_memory.contains(candidate)) {
            return false;
          }
          origin = memory_origins[candidate];
          return true;
        };

        bool reachesSink = false;
        switch (sink->selector_kind) {
        case DataSelectorKind::Return:
          reachesSink = valueIsTainted(call);
          break;
        case DataSelectorKind::Argument: {
          const Value *argument = selectedArgument(*call, *sink);
          reachesSink = valueIsTainted(argument);
          break;
        }
        case DataSelectorKind::ArgumentMemory: {
          const Value *argument = selectedArgument(*call, *sink);
          reachesSink = memoryIsTainted(argument);
          break;
        }
        case DataSelectorKind::AnyArgument:
          for (const Use &argument : call->args()) {
            if (valueIsTainted(argument.get()) ||
                memoryIsTainted(argument.get())) {
              reachesSink = true;
              break;
            }
          }
          break;
        }
        if (!reachesSink || !emitted.insert(call).second) {
          continue;
        }

        CheckerDiagnostic diagnostic;
        diagnostic.checker_id = spec.metadata.id;
        diagnostic.bug_type = spec.metadata.title;
        diagnostic.severity = spec.metadata.severity;
        diagnostic.primary_value = call;
        diagnostic.message = spec.message;
        diagnostic.suggestion = spec.suggestion;
        diagnostic.confidence = spec.confidence;
        diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
        diagnostic.metadata["sink"] = name;
        diagnostic.trace.push_back(
            CheckerTraceStep{origin, "tainted data originates here", 0});
        diagnostic.trace.push_back(CheckerTraceStep{call, spec.message, 0});
        diagnostics.push_back(std::move(diagnostic));
      }
    }
  }

  return diagnostics;
}

static const ProtocolOperation *
findProtocolOperation(StringRef name,
                      const std::vector<ProtocolOperation> &operations) {
  for (const ProtocolOperation &operation : operations) {
    if (operation.function == name) {
      return &operation;
    }
  }
  return nullptr;
}

static const Value *extractResource(const CallBase &call,
                                    const ProtocolOperation &operation) {
  if (operation.resource_kind == ResourceSelectorKind::Return) {
    return call.getType()->isVoidTy() ? nullptr : canonicalPointer(&call);
  }
  if (operation.resource_arg >= call.arg_size()) {
    return nullptr;
  }
  return canonicalPointer(call.getArgOperand(operation.resource_arg));
}

static std::vector<CheckerDiagnostic> runApiProtocol(const CheckerSpec &spec,
                                                     CheckerContext &context) {
  enum ResourceState : unsigned {
    Unknown = 0,
    Acquired = 1 << 0,
    Released = 1 << 1,
  };
  using StateMap = std::map<const Value *, unsigned>;

  struct FunctionSummary {
    std::set<unsigned> acquire_args;
    std::set<unsigned> use_args;
    std::set<unsigned> release_args;
    std::optional<unsigned> returned_argument;
    bool returns_acquired = false;

    bool operator==(const FunctionSummary &other) const {
      return acquire_args == other.acquire_args && use_args == other.use_args &&
             release_args == other.release_args &&
             returned_argument == other.returned_argument &&
             returns_acquired == other.returns_acquired;
    }
    bool operator!=(const FunctionSummary &other) const {
      return !(*this == other);
    }
  };

  std::vector<CheckerDiagnostic> diagnostics;
  std::set<std::tuple<const Instruction *, std::string, const Value *>> emitted;

  auto emit = [&](const CallBase *call, StringRef message, StringRef kind,
                  const Value *tracked) {
    const auto key = std::make_tuple(static_cast<const Instruction *>(call),
                                     kind.str(), tracked);
    if (!emitted.insert(key).second) {
      return;
    }
    CheckerDiagnostic diagnostic;
    diagnostic.checker_id = spec.metadata.id;
    diagnostic.bug_type = spec.metadata.title;
    diagnostic.severity = spec.metadata.severity;
    diagnostic.primary_value = call;
    diagnostic.message = message.str();
    diagnostic.suggestion = spec.suggestion;
    diagnostic.confidence = spec.confidence;
    diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
    diagnostic.metadata["protocol_violation"] = kind.str();
    if (tracked && tracked->hasName()) {
      diagnostic.metadata["resource"] = tracked->getName().str();
    }
    diagnostics.push_back(std::move(diagnostic));
  };

  auto mergeStates = [](StateMap &destination, const StateMap &source) {
    bool changed = false;
    for (const auto &[resource, state] : source) {
      unsigned &current = destination[resource];
      const unsigned merged = current | state;
      if (merged != current) {
        current = merged;
        changed = true;
      }
    }
    return changed;
  };

  std::map<const Function *, FunctionSummary> summaries;
  auto argumentIndex = [](const Function &function,
                          const Value *value) -> std::optional<unsigned> {
    value = canonicalPointer(value);
    for (const Argument &argument : function.args()) {
      if (canonicalPointer(&argument) == value) {
        return argument.getArgNo();
      }
    }
    return std::nullopt;
  };

  auto canonicalWithSummaries = [&](const Value *value) {
    SmallPtrSet<const Value *, 8> visited;
    while (value && visited.insert(value).second) {
      value = canonicalPointer(value);
      const auto *call = dyn_cast<CallBase>(value);
      if (!call) {
        break;
      }
      const Function *callee = directCallee(*call);
      auto summary = summaries.find(callee);
      if (summary == summaries.end() ||
          !summary->second.returned_argument.has_value()) {
        break;
      }
      const unsigned index = *summary->second.returned_argument;
      if (index >= call->arg_size()) {
        break;
      }
      value = call->getArgOperand(index);
    }
    return canonicalPointer(value);
  };

  // Build compact interprocedural typestate summaries. The fixed point lets
  // wrappers compose acquire/use/release effects through multiple call levels.
  bool summariesChanged = true;
  while (summariesChanged) {
    summariesChanged = false;
    for (Function &function : context.module) {
      if (function.isDeclaration()) {
        continue;
      }
      FunctionSummary next;
      SmallPtrSet<const Value *, 8> acquired;
      bool returnedArgumentConflict = false;
      for (Instruction &instruction : instructions(function)) {
        if (const auto *call = dyn_cast<CallBase>(&instruction)) {
          const std::string name = calleeName(*call);
          if (const ProtocolOperation *operation =
                  findProtocolOperation(name, spec.api_protocol.acquire)) {
            const Value *resource =
                canonicalWithSummaries(extractResource(*call, *operation));
            if (resource) {
              acquired.insert(resource);
              if (auto index = argumentIndex(function, resource)) {
                next.acquire_args.insert(*index);
              }
            }
            continue;
          }
          if (const ProtocolOperation *operation =
                  findProtocolOperation(name, spec.api_protocol.use)) {
            const Value *resource =
                canonicalWithSummaries(extractResource(*call, *operation));
            if (auto index = argumentIndex(function, resource)) {
              next.use_args.insert(*index);
            }
            continue;
          }
          if (const ProtocolOperation *operation =
                  findProtocolOperation(name, spec.api_protocol.release)) {
            const Value *resource =
                canonicalWithSummaries(extractResource(*call, *operation));
            if (auto index = argumentIndex(function, resource)) {
              next.release_args.insert(*index);
            }
            continue;
          }

          const Function *callee = directCallee(*call);
          auto calleeSummary = summaries.find(callee);
          if (calleeSummary == summaries.end()) {
            continue;
          }
          auto mapSummaryArgs = [&](const std::set<unsigned> &source,
                                    std::set<unsigned> &destination) {
            for (unsigned calleeIndex : source) {
              if (calleeIndex >= call->arg_size()) {
                continue;
              }
              const Value *actual =
                  canonicalWithSummaries(call->getArgOperand(calleeIndex));
              if (auto callerIndex = argumentIndex(function, actual)) {
                destination.insert(*callerIndex);
              }
            }
          };
          mapSummaryArgs(calleeSummary->second.acquire_args, next.acquire_args);
          mapSummaryArgs(calleeSummary->second.use_args, next.use_args);
          mapSummaryArgs(calleeSummary->second.release_args, next.release_args);
          if (calleeSummary->second.returns_acquired) {
            acquired.insert(canonicalPointer(call));
          }
        }

        const auto *ret = dyn_cast<ReturnInst>(&instruction);
        if (!ret || !ret->getReturnValue()) {
          continue;
        }
        const Value *returned = canonicalWithSummaries(ret->getReturnValue());
        if (auto index = argumentIndex(function, returned)) {
          if (!returnedArgumentConflict) {
            if (!next.returned_argument.has_value()) {
              next.returned_argument = *index;
            } else if (*next.returned_argument != *index) {
              next.returned_argument.reset();
              returnedArgumentConflict = true;
            }
          }
        } else {
          next.returned_argument.reset();
          returnedArgumentConflict = true;
        }
        if (acquired.contains(returned)) {
          next.returns_acquired = true;
        }
      }
      if (summaries[&function] != next) {
        summaries[&function] = std::move(next);
        summariesChanged = true;
      }
    }
  }

  for (Function &function : context.module) {
    if (function.isDeclaration()) {
      continue;
    }

    std::map<const BasicBlock *, StateMap> in_states;
    std::map<const BasicBlock *, StateMap> out_states;
    std::set<const Value *> acquired_resources;
    std::queue<const BasicBlock *> worklist;
    SmallPtrSet<const BasicBlock *, 16> queued;
    SmallPtrSet<const BasicBlock *, 16> processed;
    worklist.push(&function.getEntryBlock());
    queued.insert(&function.getEntryBlock());

    auto transferBlock = [&](const BasicBlock *block, StateMap &state,
                             bool emit_diagnostics) {
      for (const Instruction &instruction : *block) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        if (!call) {
          continue;
        }
        const std::string name = calleeName(*call);

        auto acquireResource = [&](const Value *resource) {
          resource = canonicalWithSummaries(resource);
          if (!resource) {
            return;
          }
          const unsigned previous = state[resource];
          if (emit_diagnostics && (previous & Acquired) &&
              spec.api_protocol.report_double_acquire) {
            emit(call, spec.message + " (double acquire)", "double-acquire",
                 resource);
          }
          state[resource] = Acquired;
          acquired_resources.insert(resource);
        };
        auto useResource = [&](const Value *resource) {
          resource = canonicalWithSummaries(resource);
          if (!resource) {
            return;
          }
          const unsigned current = state[resource];
          if (emit_diagnostics && current == Unknown &&
              spec.api_protocol.report_use_before_acquire) {
            emit(call, spec.message + " (use before acquire)",
                 "use-before-acquire", resource);
          } else if (emit_diagnostics && (current & Released) &&
                     spec.api_protocol.report_use_after_release) {
            emit(call, spec.message + " (use after release)",
                 "use-after-release", resource);
          }
        };
        auto releaseResource = [&](const Value *resource) {
          resource = canonicalWithSummaries(resource);
          if (!resource) {
            return;
          }
          const unsigned previous = state[resource];
          if (emit_diagnostics && (previous & Released) &&
              spec.api_protocol.report_double_release) {
            emit(call, spec.message + " (double release)", "double-release",
                 resource);
          }
          state[resource] = Released;
        };

        if (const ProtocolOperation *operation =
                findProtocolOperation(name, spec.api_protocol.acquire)) {
          acquireResource(extractResource(*call, *operation));
          continue;
        }

        if (const ProtocolOperation *operation =
                findProtocolOperation(name, spec.api_protocol.use)) {
          useResource(extractResource(*call, *operation));
          continue;
        }

        if (const ProtocolOperation *operation =
                findProtocolOperation(name, spec.api_protocol.release)) {
          releaseResource(extractResource(*call, *operation));
          continue;
        }

        const Function *callee = directCallee(*call);
        auto summary = summaries.find(callee);
        if (summary == summaries.end()) {
          continue;
        }
        for (unsigned index : summary->second.acquire_args) {
          if (index < call->arg_size()) {
            acquireResource(call->getArgOperand(index));
          }
        }
        for (unsigned index : summary->second.use_args) {
          if (index < call->arg_size()) {
            useResource(call->getArgOperand(index));
          }
        }
        for (unsigned index : summary->second.release_args) {
          if (index < call->arg_size()) {
            releaseResource(call->getArgOperand(index));
          }
        }
        if (summary->second.returns_acquired && !call->getType()->isVoidTy()) {
          acquireResource(call);
        }
      }
    };

    while (!worklist.empty()) {
      const BasicBlock *block = worklist.front();
      worklist.pop();
      queued.erase(block);

      StateMap state = in_states[block];
      transferBlock(block, state, false);

      if (processed.contains(block) && out_states[block] == state) {
        continue;
      }
      processed.insert(block);
      out_states[block] = std::move(state);
      for (const BasicBlock *successor : successors(block)) {
        const bool inputChanged =
            mergeStates(in_states[successor], out_states[block]);
        if ((inputChanged || !processed.contains(successor)) &&
            queued.insert(successor).second) {
          worklist.push(successor);
        }
      }
    }

    for (const BasicBlock &block : function) {
      StateMap state = in_states[&block];
      transferBlock(&block, state, true);
    }

    if (!spec.api_protocol.report_leak) {
      continue;
    }
    for (const BasicBlock &block : function) {
      const auto *returnInst = dyn_cast<ReturnInst>(block.getTerminator());
      if (!returnInst) {
        continue;
      }
      const StateMap &state = out_states[&block];
      const Value *returnedResource =
          returnInst->getReturnValue()
              ? canonicalWithSummaries(returnInst->getReturnValue())
              : nullptr;
      for (const Value *resource : acquired_resources) {
        if (returnedResource == resource) {
          continue;
        }
        const auto it = state.find(resource);
        if (it == state.end() || !(it->second & Acquired)) {
          continue;
        }
        const auto key = std::make_tuple(block.getTerminator(),
                                         std::string("leak"), resource);
        if (!emitted.insert(key).second) {
          continue;
        }
        CheckerDiagnostic diagnostic;
        diagnostic.checker_id = spec.metadata.id;
        diagnostic.bug_type = spec.metadata.title;
        diagnostic.severity = spec.metadata.severity;
        diagnostic.primary_value = resource;
        diagnostic.message = spec.message + " (resource leak)";
        diagnostic.suggestion = spec.suggestion;
        diagnostic.confidence = spec.confidence;
        diagnostic.metadata["rule_kind"] = toString(spec.rule_kind);
        diagnostic.metadata["protocol_violation"] = "leak";
        diagnostics.push_back(std::move(diagnostic));
      }
    }
  }

  return diagnostics;
}

static Expected<std::vector<CheckerDiagnostic>>
runDeclarative(const CheckerSpec &spec, CheckerContext &context) {
  switch (spec.rule_kind) {
  case RuleKind::ForbiddenCall:
    return runForbiddenCall(spec, context);
  case RuleKind::SourceSink:
    return runSourceSink(spec, context);
  case RuleKind::ApiProtocol:
    return runApiProtocol(spec, context);
  case RuleKind::Native:
    return createStringError(inconvertibleErrorCode(),
                             "native rule kind is not executable");
  }
  return createStringError(inconvertibleErrorCode(),
                           "unhandled declarative rule kind");
}

} // namespace

Expected<std::vector<CheckerDiagnostic>> CheckerDriver::run(
    const std::vector<const CheckerDescriptor *> &selection) const {
  std::vector<CheckerDiagnostic> diagnostics;
  for (const CheckerDescriptor *descriptor : selection) {
    if (descriptor == nullptr) {
      continue;
    }
    if (!descriptor->isDeclarative()) {
      return createStringError(inconvertibleErrorCode(),
                               "checker '%s' is not executable via lotus-check",
                               descriptor->metadata.id.c_str());
    }
    auto diagnostics_or = runDeclarative(*descriptor->spec, context_);
    if (!diagnostics_or) {
      return diagnostics_or.takeError();
    }
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(diagnostics_or->begin()),
                       std::make_move_iterator(diagnostics_or->end()));
  }
  return diagnostics;
}

Error CheckerDriver::emitToReportManager(
    const std::vector<CheckerDiagnostic> &diagnostics) const {
  BugReportMgr &manager = BugReportMgr::get_instance();
  manager.clear_all_reports();

  std::map<std::string, Severity> bug_type_severities;
  for (const auto &diagnostic : diagnostics) {
    Severity &severity = bug_type_severities[diagnostic.bug_type];
    if (static_cast<int>(diagnostic.severity) > static_cast<int>(severity)) {
      severity = diagnostic.severity;
    }
  }

  std::map<std::string, int> bug_type_ids;
  for (const auto &diagnostic : diagnostics) {
    auto it = bug_type_ids.find(diagnostic.bug_type);
    if (it == bug_type_ids.end()) {
      int bug_type_id = manager.register_bug_type(
          diagnostic.bug_type,
          severityToImportance(bug_type_severities[diagnostic.bug_type]));
      it = bug_type_ids.emplace(diagnostic.bug_type, bug_type_id).first;
    }
    manager.insert_report(it->second, diagnostic.toBugReport(it->second), true);
  }

  return Error::success();
}

} // namespace lotus::checker

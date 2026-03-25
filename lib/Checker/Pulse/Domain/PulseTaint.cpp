
#include "Checker/Pulse/Domain/PulseTaint.h"

#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <llvm/IR/Instructions.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// Taint
//
// Taint is an auxiliary analysis used by Pulse to detect source->sink flows.
//
// Sound incorrectness note:
// - A taint report is ideally backed by a concrete feasible witness path.
// - When modeling is incomplete (e.g., unknown/indirect calls), propagation may
//   become conservative and can introduce false positives. Tighten behavior via
//   `PulseTaintConfig` and models.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// TaintDomain Implementation
//===----------------------------------------------------------------------===//

void TaintDomain::add(AbstractValue v, TaintItem item) {
  taints_[v].insert(item);
}

void TaintDomain::remove(AbstractValue v) { taints_.erase(v); }

bool TaintDomain::has(AbstractValue v) const {
  return taints_.find(v) != taints_.end();
}

const std::set<TaintItem> &TaintDomain::get(AbstractValue v) const {
  auto it = taints_.find(v);
  static const std::set<TaintItem> empty;
  return (it != taints_.end()) ? it->second : empty;
}

void TaintDomain::join(const TaintDomain &other) {
  for (const auto &kv : other.taints_) {
    taints_[kv.first].insert(kv.second.begin(), kv.second.end());
  }
}

//===----------------------------------------------------------------------===//
// TaintOperations Implementation
//===----------------------------------------------------------------------===//

unsigned TaintOperations::global_timestamp_ = 1;
const TaintConfig *TaintOperations::config_ = nullptr;

void TaintOperations::taint(AbductiveDomain &astate, AbstractValue v,
                            const std::vector<TaintKind> &kinds,
                            const llvm::Instruction *source) {
  const llvm::Function *func = nullptr;
  if (source) {
    if (const llvm::BasicBlock *bb = source->getParent())
      func = bb->getParent();
  }
  unsigned timestamp = getNextTimestamp();

  // Create value tuple
  TaintValue value(TaintValue::Type::TaintProcedure,
                   func ? func->getName().str() : "");
  TaintValueTuple value_tuple(value, TaintOrigin::ReturnValue);

  TaintItem item(kinds, value_tuple, source, func, timestamp);
  item.history.addEvent(ValueHistory::EventKind::Unknown, source,
                        func); // Start of taint
  astate.getTaintDomain().add(v, item);
  astate.getPostAttrs().add(v, Attribute::Tainted);
}

void TaintOperations::taint(AbductiveDomain &astate, AbstractValue v,
                            TaintKind kind, const std::string &procedure_name) {
  unsigned timestamp = getNextTimestamp();
  TaintValue value(TaintValue::Type::TaintProcedure, procedure_name);
  TaintValueTuple value_tuple(value, TaintOrigin::ReturnValue);
  TaintItem item({kind}, value_tuple, nullptr, nullptr, timestamp);
  astate.getTaintDomain().add(v, item);
  astate.getPostAttrs().add(v, Attribute::Tainted);
}

bool TaintOperations::checkSink(AbductiveDomain &astate, AbstractValue v,
                                const std::string &sink_name,
                                const llvm::Instruction *sink_loc) {
  if (!astate.getTaintDomain().has(v)) {
    return false;
  }

  const auto &taints = astate.getTaintDomain().get(v);
  if (taints.empty())
    return false;

  // Get sink policies from config
  const TaintConfig &config = getConfig();
  TaintKind sink_kind =
      TaintKind::Unknown(); // Would be determined from sink_name
  const auto &policies = config.getSinkPolicies(sink_kind);

  // Find unsanitized taint items that match sink policies
  std::vector<const TaintItem *> unsanitized_taints;
  for (const auto &item : taints) {
    // Check if sanitized by any policy's sanitizer kinds
    bool sanitized = false;
    for (const auto &policy : policies) {
      if (item.isSanitizedBy(policy.sanitizer_kinds)) {
        sanitized = true;
        break;
      }
    }

    if (!sanitized) {
      // Check if item's kinds match policy's source kinds
      for (const auto &policy : policies) {
        for (const auto &item_kind : item.kinds) {
          for (const auto &source_kind : policy.source_kinds) {
            if (item_kind == source_kind) {
              unsanitized_taints.push_back(&item);
              goto next_item;
            }
          }
        }
      }
    }
  next_item:;
  }

  if (unsanitized_taints.empty()) {
    return false; // All taints are sanitized or don't match policies
  }

  // Report the most critical one
  const TaintItem *critical_item = nullptr;
  TaintKind critical_kind = TaintKind::Unknown();

  for (const auto *item : unsanitized_taints) {
    TaintKind primary = item->getPrimaryKind();
    // Prioritize: Sensitive > Network > UserInput > FileSystem > Environment >
    // Unknown
    if (!critical_item) {
      critical_item = item;
      critical_kind = primary;
    } else if (primary == TaintKind::Sensitive()) {
      critical_item = item;
      critical_kind = primary;
    } else if (critical_kind != TaintKind::Sensitive() &&
               primary == TaintKind::Network()) {
      critical_item = item;
      critical_kind = primary;
    } else if (critical_kind == TaintKind::Unknown() &&
               primary != TaintKind::Unknown()) {
      critical_item = item;
      critical_kind = primary;
    }
  }

  if (!critical_item) {
    return false;
  }

  const TaintItem &item = *critical_item;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  // Register type if not already (hacky, should be in
  // PulseChecker::registerBugTypes)
  int typeId = mgr.register_bug_type("Taint Flow", BugDescription::BI_HIGH,
                                     BugDescription::BC_SECURITY, "CWE-20");

  BugReport *report = new BugReport(typeId);

  // Add sink step with detailed information
  std::string sink_msg = "Tainted data flows into sink '";
  sink_msg += sink_name;
  sink_msg += "'";
  if (sink_name == "system" || sink_name == "exec" || sink_name == "popen") {
    sink_msg += " (command injection risk)";
  } else if (sink_name == "printf" || sink_name == "sprintf") {
    sink_msg += " (format string vulnerability)";
  } else if (sink_name == "strcpy" || sink_name == "strcat") {
    sink_msg += " (buffer overflow risk)";
  }

  report->append_step(const_cast<llvm::Instruction *>(sink_loc), sink_msg, 0,
                      {}, "sink");

  // Add trace from history (production-ready: show full propagation path)
  const auto &events = item.history.getEvents();
  unsigned step_num = 1;
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->location) {
      std::string trace_msg = "Taint propagated here";
      if (it->kind == ValueHistory::EventKind::Store) {
        trace_msg += " (via store)";
      } else if (it->kind == ValueHistory::EventKind::Load) {
        trace_msg += " (via load)";
      } else if (it->kind == ValueHistory::EventKind::FunctionCall) {
        trace_msg += " (via function call)";
      }
      report->append_step(const_cast<llvm::Instruction *>(it->location),
                          trace_msg, step_num++, {}, "trace");
    }
  }

  // Add source step with detailed information
  if (item.source_instruction) {
    std::string src_msg = "Taint source: ";
    TaintKind primary = item.getPrimaryKind();
    if (primary == TaintKind::Network()) {
      src_msg += "Network input";
    } else if (primary == TaintKind::UserInput()) {
      src_msg += "User input";
    } else if (primary == TaintKind::FileSystem()) {
      src_msg += "File system";
    } else if (primary == TaintKind::Environment()) {
      src_msg += "Environment variable";
    } else if (primary == TaintKind::Sensitive()) {
      src_msg += "Sensitive data";
    } else {
      src_msg += "Unknown source";
    }

    report->append_step(
        const_cast<llvm::Instruction *>(item.source_instruction), src_msg,
        step_num++, {}, "source");
  }

  // Set confidence score based on taint kind and sink type
  int confidence = 80;
  if (critical_kind == TaintKind::Sensitive() ||
      critical_kind == TaintKind::Network()) {
    confidence = 95;
  } else if (critical_kind == TaintKind::UserInput()) {
    confidence = 90;
  }

  // Increase confidence for dangerous sinks
  if (sink_name == "system" || sink_name == "exec") {
    confidence = std::min(95, confidence + 5);
  }

  report->set_conf_score(confidence);
  mgr.insert_report(typeId, report, true);
  return true;
}

void TaintOperations::propagate(AbductiveDomain &astate, AbstractValue src,
                                AbstractValue dest,
                                const llvm::Instruction *loc) {
  if (!astate.getTaintDomain().has(src)) {
    return;
  }

  const auto &src_taints = astate.getTaintDomain().get(src);
  for (const auto &item : src_taints) {
    // Create a copy and update history
    TaintItem new_item = item;
    new_item.history.addEvent(ValueHistory::EventKind::Store, loc,
                              loc ? loc->getFunction() : nullptr);
    astate.getTaintDomain().add(dest, new_item);
  }
  astate.getPostAttrs().add(dest, Attribute::Tainted);
}

void TaintOperations::sanitize(AbductiveDomain &astate, AbstractValue v,
                               TaintKind sanitizer_kind,
                               const llvm::Instruction *sanitizer_loc) {
  if (!astate.getTaintDomain().has(v)) {
    return;
  }

  unsigned sanitizer_timestamp = getNextTimestamp();
  auto &taints =
      const_cast<std::set<TaintItem> &>(astate.getTaintDomain().get(v));

  // Add sanitizer to all taint items
  for (auto &item : taints) {
    const_cast<TaintItem &>(item).addSanitizer(sanitizer_kind, sanitizer_loc,
                                               sanitizer_timestamp);
  }

  // Sanitizers are tracked in TaintItem, no separate attribute needed
}

bool TaintOperations::isSanitized(const AbductiveDomain &astate,
                                  AbstractValue v) {
  if (!astate.getTaintDomain().has(v)) {
    return false;
  }

  const auto &taints = astate.getTaintDomain().get(v);
  for (const auto &item : taints) {
    if (!item.isSanitized()) {
      return false; // At least one unsanitized taint
    }
  }
  return true;
}

void TaintOperations::propagateThroughLoad(AbductiveDomain &astate,
                                           AbstractValue src_addr,
                                           AbstractValue dest_val,
                                           const llvm::Instruction *loc) {
  // Propagate taint from memory location to loaded value
  propagate(astate, src_addr, dest_val, loc);
}

void TaintOperations::propagateThroughStore(AbductiveDomain &astate,
                                            AbstractValue src_val,
                                            AbstractValue dest_addr,
                                            const llvm::Instruction *loc) {
  // Propagate taint from stored value to memory location
  propagate(astate, src_val, dest_addr, loc);
}

void TaintOperations::propagateThroughCall(
    AbductiveDomain &astate, const llvm::CallInst *call,
    const std::vector<AbstractValue> &args, AbstractValue ret_val) {
  // Use TaintConfig for source/sink/sanitizer detection
  const TaintConfig &config = getConfig();
  const llvm::Function *callee = call->getCalledFunction();

  if (!callee) {
    // Indirect call - conservatively propagate all taints
    for (AbstractValue arg : args) {
      if (astate.getTaintDomain().has(arg)) {
        propagate(astate, arg, ret_val, call);
      }
    }
    return;
  }

  // Check for taint sources using config
  std::vector<TaintKind> source_kinds = config.isSource(callee);
  if (!source_kinds.empty()) {
    taint(astate, ret_val, source_kinds, call);
    return; // Sources introduce new taint, don't propagate from args
  }

  // Check for taint sinks using config
  std::vector<TaintKind> sink_kinds;
  if (config.isSink(callee, sink_kinds)) {
    // Check all pointer arguments for taint
    for (size_t i = 0; i < args.size() && i < call->arg_size(); ++i) {
      const llvm::Value *arg_val = call->getArgOperand(i);
      if (arg_val->getType()->isPointerTy()) {
        AbstractValue arg_av = args[i];
        if (astate.getTaintDomain().has(arg_av)) {
          checkSink(astate, arg_av, callee->getName().str(), call);
        }
      }
    }
  }

  // Check for sanitizers using config
  std::vector<TaintKind> sanitizer_kinds = config.isSanitizer(callee);
  if (!sanitizer_kinds.empty() && !args.empty()) {
    // Sanitize the first argument
    for (TaintKind kind : sanitizer_kinds) {
      sanitize(astate, args[0], kind, call);
    }
    return; // Don't propagate taint from sanitizers
  }

  // Check for propagators
  if (config.isPropagator(callee)) {
    // Propagate taint from all arguments to return value
    for (AbstractValue arg : args) {
      if (astate.getTaintDomain().has(arg)) {
        propagate(astate, arg, ret_val, call);
      }
    }
    return;
  }

  // Default: conservatively propagate taint from all arguments to return value
  for (AbstractValue arg : args) {
    if (astate.getTaintDomain().has(arg)) {
      propagate(astate, arg, ret_val, call);
    }
  }
}

} // namespace pulse

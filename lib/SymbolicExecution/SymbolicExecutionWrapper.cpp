//===----------------------------------------------------------------------===//
//
// SymbolicExecutionWrapper is the bridge from the internal symbolic executor to
// Lotus's checker reporting pipeline. It runs the driver as a legacy module
// pass, then turns taint and execution traces into BugReport objects with the
// metadata and step labels expected by downstream consumers.
//
//===----------------------------------------------------------------------===//

#include "SymbolicExecution/SymbolicExecutionWrapper.h"

#include "llvm/IR/Instructions.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/LotusAdapter.h"
#include "SymbolicExecution/GVFGUtility.h"

#include <string>
#include <vector>

using namespace llvm;
using namespace SymbolicExecution;

#define DEBUG_TYPE "SymbolicExecutionWrapper"

namespace {

struct SymexBugTypeInfo {
  const char *name;
  BugDescription::BugImportance importance;
  BugDescription::BugClassification classification;
  const char *cwe;
  const char *primary_message;
};

SymexBugTypeInfo getBugTypeInfo(AnalysisState::SymexBugType bug_type) {
  // Keep the mapping from internal bug kind to external report vocabulary in a
  // single table-like switch. The driver and AnalysisState only traffic in the
  // enum, while the wrapper owns presentation details such as CWE and severity.
  switch (bug_type) {
  case AnalysisState::BUG_TY_BOF:
    return {"Symbolic Execution Buffer Overflow", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-120, CWE-122",
            "Potential buffer overflow"};
  case AnalysisState::BUG_TY_DBZ:
    return {"Symbolic Execution Divide by Zero", BugDescription::BI_MEDIUM,
            BugDescription::BC_ERROR, "CWE-369", "Potential divide by zero"};
  case AnalysisState::BUG_TY_INT_OVERFLOW:
    return {"Symbolic Execution Integer Overflow", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-190",
            "Potential integer overflow"};
  case AnalysisState::BUG_TY_INT_UNDERFLOW:
    return {"Symbolic Execution Integer Underflow", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-191",
            "Potential integer underflow"};
  case AnalysisState::BUG_TY_NULL_DEREF:
    return {"Symbolic Execution Null Dereference", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-476, CWE-690",
            "Potential null dereference"};
  case AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW:
    return {"Symbolic Execution Signed Integer Overflow",
            BugDescription::BI_HIGH, BugDescription::BC_SECURITY, "CWE-190",
            "Potential signed integer overflow"};
  case AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW:
    return {"Symbolic Execution Signed Integer Underflow",
            BugDescription::BI_HIGH, BugDescription::BC_SECURITY, "CWE-191",
            "Potential signed integer underflow"};
  case AnalysisState::BUG_TY_SHIFT_OVERFLOW:
    return {"Symbolic Execution Shift Overflow", BugDescription::BI_MEDIUM,
            BugDescription::BC_ERROR, "CWE-1335",
            "Potential invalid shift operation"};
  case AnalysisState::BUG_TY_ARRAY_INDEX_OOB:
    return {"Symbolic Execution Array Index Out of Bounds",
            BugDescription::BI_HIGH, BugDescription::BC_SECURITY,
            "CWE-129, CWE-787", "Potential array index out of bounds"};
  case AnalysisState::BUG_TY_UNINIT_READ:
    return {"Symbolic Execution Uninitialized Read", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-457",
            "Potential uninitialized read"};
  case AnalysisState::BUG_TY_UAF:
    return {"Symbolic Execution Use After Free", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-416", "Potential use after free"};
  case AnalysisState::BUG_TY_DOUBLE_FREE:
    return {"Symbolic Execution Double Free", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-415", "Potential double free"};
  case AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX:
    return {"Symbolic Execution Negative Array Index", BugDescription::BI_HIGH,
            BugDescription::BC_SECURITY, "CWE-129, CWE-839",
            "Potential negative array index"};
  case AnalysisState::BUG_TY_INT_TRUNCATION:
    return {"Symbolic Execution Integer Truncation", BugDescription::BI_MEDIUM,
            BugDescription::BC_WARNING, "CWE-197",
            "Potential integer truncation"};
  case AnalysisState::BUG_TY_UNDEF:
    break;
  }

  return {"Symbolic Execution Issue", BugDescription::BI_NA,
          BugDescription::BC_NA, "", "Potential symbolic execution issue"};
}

std::string renderValueLabel(const Value *value) {
  if (!value) {
    return "value";
  }

  if (value->hasName()) {
    return value->getName().str();
  }

  std::string printed;
  raw_string_ostream os(printed);
  value->printAsOperand(os, false);
  return os.str();
}

std::vector<NodeTag> getNodeTags(const Instruction *inst, bool is_call_step) {
  std::vector<NodeTag> tags;
  if (is_call_step || isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
    tags.push_back(NodeTag::CALL_SITE);
  }
  return tags;
}

std::string getInstructionAccess(const Instruction *inst,
                                 llvm::StringRef fallback) {
  if (!inst) {
    return fallback.str();
  }
  if (isa<LoadInst>(inst)) {
    return "load";
  }
  if (isa<StoreInst>(inst)) {
    return "store";
  }
  if (isa<CallInst>(inst) || isa<InvokeInst>(inst)) {
    return "call";
  }
  return fallback.str();
}

std::string renderTraceMessage(const TraceStep &step) {
  switch (step.TK) {
  case TraceStep::TRACE_STEP_CALL:
    return "Call site on the reported path";
  case TraceStep::TRACE_STEP_ALLOC:
    return "Memory object related to this report is allocated here";
  case TraceStep::TRACE_STEP_BUFFER_ACCESS:
    return "Bounds-relevant access involving " + renderValueLabel(step.Val);
  case TraceStep::TRACE_STEP_DIV:
    return "Division uses " + renderValueLabel(step.Val);
  case TraceStep::TRACE_STEP_ARITH:
    return "Arithmetic step involving " + renderValueLabel(step.Val);
  case TraceStep::TRACE_STEP_DEREF:
    return "Pointer dereference involving " + renderValueLabel(step.Val);
  }

  return "Symbolic execution trace step";
}

std::string getTraceAccess(const TraceStep &step) {
  switch (step.TK) {
  case TraceStep::TRACE_STEP_CALL:
    return "call";
  case TraceStep::TRACE_STEP_ALLOC:
    return "allocation";
  case TraceStep::TRACE_STEP_BUFFER_ACCESS:
    return getInstructionAccess(step.Inst, "buffer");
  case TraceStep::TRACE_STEP_DIV:
    return "divide";
  case TraceStep::TRACE_STEP_ARITH:
    return "arithmetic";
  case TraceStep::TRACE_STEP_DEREF:
    return getInstructionAccess(step.Inst, "dereference");
  }

  return "trace";
}

std::string renderTaintMessage(const TaintStep &step) {
  switch (step.TK) {
  case TaintStep::TAINT_STEP_SOURCE:
    return "Taint source reaches " + renderValueLabel(step.V1);
  case TaintStep::TAINT_STEP_CALL:
    return "Taint crosses this call boundary";
  case TaintStep::TAINT_STEP_PROP:
    return "Taint propagates from " + renderValueLabel(step.V1) + " to " +
           renderValueLabel(step.V2);
  }

  return "Taint-related step";
}

std::string getTaintAccess(const TaintStep &step) {
  switch (step.TK) {
  case TaintStep::TAINT_STEP_SOURCE:
    return "source";
  case TaintStep::TAINT_STEP_CALL:
    return "call";
  case TaintStep::TAINT_STEP_PROP:
    return getInstructionAccess(step.Inst, "propagation");
  }

  return "taint";
}

Instruction *
getPrimaryBugInstruction(const std::vector<TraceStep> &trace_steps,
                         const std::vector<TaintStep> &taint_steps) {
  if (!trace_steps.empty()) {
    return trace_steps.front().Inst;
  }
  if (!taint_steps.empty()) {
    return taint_steps.back().Inst;
  }
  return nullptr;
}

} // namespace

char llvm::SymbolicExecutionWrapper::ID = 0;
static RegisterPass<llvm::SymbolicExecutionWrapper>
    X(DEBUG_TYPE, "Lotus symbolic execution wrapper");

llvm::SymbolicExecutionWrapper::SymbolicExecutionWrapper() : ModulePass(ID) {}
llvm::SymbolicExecutionWrapper::~SymbolicExecutionWrapper() = default;

int llvm::SymbolicExecutionWrapper::getBugTypeId(
    SymbolicExecution::AnalysisState::SymexBugType bug_type) {
  const auto info = getBugTypeInfo(bug_type);
  return BugReportMgr::get_instance().register_bug_type(
      info.name, info.importance, info.classification, info.cwe);
}

void llvm::SymbolicExecutionWrapper::emitBugReports(
    const SymbolicExecution::AnalysisDriver &driver) const {
  // emitBugReports materializes the final user-facing report. Taint steps are
  // emitted first so source-to-sink context is visible before the execution
  // trace is replayed back toward the primary bug instruction.
  auto bug_traces = driver.getBugTraces();
  auto &mgr = BugReportMgr::get_instance();

  for (const auto &bug_trace : bug_traces) {
    const auto bug_type = std::get<0>(bug_trace);
    const auto &taint_steps = std::get<1>(bug_trace);
    const auto &trace_steps = std::get<2>(bug_trace);

    if (bug_type == AnalysisState::BUG_TY_UNDEF) {
      continue;
    }

    const auto info = getBugTypeInfo(bug_type);
    const int bug_type_id = getBugTypeId(bug_type);
    if (bug_type_id < 0) {
      continue;
    }

    auto *report = new BugReport(bug_type_id);

    int trace_level = 0;
    for (const auto &step : taint_steps) {
      report->append_step(
          step.Inst, renderTaintMessage(step), trace_level,
          getNodeTags(step.Inst, step.TK == TaintStep::TAINT_STEP_CALL),
          getTaintAccess(step));
      if (step.TK == TaintStep::TAINT_STEP_CALL) {
        ++trace_level;
      }
    }

    for (auto it = trace_steps.rbegin(); it != trace_steps.rend(); ++it) {
      if (it->Inst == nullptr) {
        continue;
      }
      // The internal trace stores the sink separately. Skip the degenerate
      // endpoints here, then append one canonical primary bug step below.
      if (it == trace_steps.rbegin() && trace_steps.size() == 1) {
        continue;
      }
      if (it == trace_steps.rend() - 1) {
        continue;
      }

      report->append_step(
          it->Inst, renderTraceMessage(*it), trace_level,
          getNodeTags(it->Inst, it->TK == TraceStep::TRACE_STEP_CALL),
          getTraceAccess(*it));
      if (it->TK == TraceStep::TRACE_STEP_CALL) {
        ++trace_level;
      }
    }

    Instruction *bug_inst = getPrimaryBugInstruction(trace_steps, taint_steps);
    report->append_step(bug_inst, info.primary_message, trace_level,
                        getNodeTags(bug_inst, false), "bug");
    report->set_conf_score(85);
    report->add_metadata("checker", "SymbolicExecutionWrapper");
    report->add_metadata("engine", "SymbolicExecution");
    if (*info.cwe != '\0') {
      report->add_metadata("cwe", info.cwe);
    }

    mgr.insert_report(bug_type_id, report, true);
  }
}

void llvm::SymbolicExecutionWrapper::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<gsa::ControlDependenceAnalysisPass>();
  AU.addRequired<gsa::GateAnalysisPass>();
  AU.addRequired<lotus::gvfg::GuardedValueFlowGraphBuilderPass>();
  AU.addRequired<lotus::gvfg::LotusGuardedValueFlowAdapterPass>();
}

bool llvm::SymbolicExecutionWrapper::runOnModule(Module &M) {
  // The wrapper owns pass-pipeline integration and report emission only. All
  // symbolic state propagation and summary scheduling live underneath in
  // AnalysisDriver and AnalysisState.
  auto &builder = getAnalysis<lotus::gvfg::GuardedValueFlowGraphBuilderPass>();
  gvfg_utility::initAnalysisInterface(&M, &M.getDataLayout(), &builder);

  AnalysisDriver driver;
  driver.runOnModule(&M);
  emitBugReports(driver);
  return false;
}

//===- DoubleFreeChecker.cpp -- Double free detector
//--------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/DoubleFreeChecker.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/BugTypes.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/SVFG/SVFGNode.h"

#include <cassert>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;

static const llvm::Value *getReportValue(const SVFGNode *node) {
  return node ? static_cast<const llvm::Value *>(node->getInstruction())
              : nullptr;
}

static void appendPathConditionEvents(BugReport *report,
                                      const ProgSlice *slice) {
  if (!report || !slice)
    return;
  ProgSlice::EventStack events;
  slice->evalFinalCond2Event(events);
  for (const auto &e : events) {
    if (!e.first)
      continue;
    const std::vector<NodeTag> tags = {e.second ? NodeTag::CONDITION_TRUE
                                                : NodeTag::CONDITION_FALSE};
    report->append_step(const_cast<Instruction *>(e.first), "Path condition", 0,
                        tags);
  }
}

void DoubleFreeChecker::reportBug(ProgSlice *slice) {
  const SVFGNode *source = slice->getSource();
  if (!source)
    return;
  const llvm::CallBase *sourceCall = getSrcCSID(source);
  const llvm::Value *reportSource =
      sourceCall ? static_cast<const llvm::Value *>(sourceCall)
                 : static_cast<const llvm::Value *>(source->getInstruction());
  const llvm::Value *reportSink = nullptr;
  if (slice && slice->sinksBegin() != slice->sinksEnd()) {
    reportSink = getReportValue(*slice->sinksBegin());
  }

  // Match SVF: only report when a double-free path exists (two free sinks
  // reachable on the same path).
  if (slice->isSatisfiableForPairs())
    return;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int bugTypeId = mgr.register_bug_type("Double Free", BugDescription::BI_HIGH,
                                        BugDescription::BC_SECURITY, "CWE-415");

  BugReport *report = new BugReport(bugTypeId);

  if (reportSource) {
    report->append_step(const_cast<Value *>(reportSource),
                        "Memory allocated here");
  }
  appendPathConditionEvents(report, slice);
  if (reportSink) {
    report->append_step(const_cast<Value *>(reportSink),
                        "Memory may be freed again here");
  } else if (reportSource) {
    report->append_step(const_cast<Value *>(reportSource),
                        "Memory may be freed twice on this path");
  }

  mgr.insert_report(bugTypeId, report, false);

  if (SaberValidateTests)
    testsValidation(slice);

  outs() << "Double Free detected at ";
  if (const auto *inst = dyn_cast_or_null<Instruction>(reportSource)) {
    if (const Function *F = inst->getFunction()) {
      outs() << F->getName();
    }
  }
  outs() << "\n";
}

void DoubleFreeChecker::testsValidation(ProgSlice *slice) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;
  const llvm::CallBase *cs = getSrcCSID(source);
  if (!cs)
    return;
  const llvm::Function *fun = cs->getCalledFunction();
  if (!fun)
    return;
  const std::string funName = fun->getName().str();
  validateSuccessTests(slice, funName);
  validateExpectedFailureTests(slice, funName);
}

void DoubleFreeChecker::validateSuccessTests(ProgSlice *slice,
                                             const std::string &fun) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;

  bool success = false;
  if (fun == "SAFEMALLOC") {
    success = slice->isSatisfiableForPairs();
  } else if (fun == "DOUBLEFREEMALLOC") {
    success = !slice->isSatisfiableForPairs();
  } else if (fun == "DOUBLEFREEMALLOCFN" || fun == "SAFEMALLOCFP") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (success) {
    outs() << "\t SUCCESS :" << srcFun << " (src id:" << source->getId()
           << ")\n";
    outs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
    return;
  }
  errs() << "\t FAILURE :" << srcFun << " (src id:" << source->getId() << ")\n";
  errs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
  assert(false && "SABER double-free validation failed");
}

void DoubleFreeChecker::validateExpectedFailureTests(ProgSlice *slice,
                                                     const std::string &fun) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;

  bool expectedFailure = false;
  if (fun == "DOUBLEFREEMALLOCFN") {
    expectedFailure = slice->isSatisfiableForPairs();
  } else if (fun == "SAFEMALLOCFP") {
    expectedFailure = !slice->isSatisfiableForPairs();
  } else if (fun == "SAFEMALLOC" || fun == "DOUBLEFREEMALLOC") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (expectedFailure) {
    outs() << "\t EXPECTED-FAILURE :" << srcFun
           << " (src id:" << source->getId() << ")\n";
    outs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
    return;
  }
  errs() << "\t UNEXPECTED FAILURE :" << srcFun
         << " (src id:" << source->getId() << ")\n";
  errs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
  assert(false && "SABER double-free unexpected validation result");
}

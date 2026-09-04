/**
 * @file PulseCheckerTest.cpp
 * @brief Behavioral tests for the Pulse checker.
 *
 * These tests focus on checker-visible outcomes:
 * - concrete bug reports emitted through BugReportMgr
 * - soundness-oriented regressions around latent issues and modeling
 * - summary and loop behavior that affects witness construction
 */

#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

#include <iterator>

using namespace llvm;
using namespace pulse;
using lotus::unittest::findCallTo;
using lotus::unittest::parseModule;

namespace {

size_t getReportCountForType(BugReportMgr &mgr, StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0) {
    return 0;
  }
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  return reports ? reports->size() : 0;
}

const BugReport *getLastReportForType(BugReportMgr &mgr,
                                      StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0) {
    return nullptr;
  }
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  if (!reports || reports->empty()) {
    return nullptr;
  }
  return reports->back();
}

bool reportContainsTip(const BugReport *report, StringRef tipSubstring) {
  if (!report) {
    return false;
  }
  for (const BugDiagStep *step : report->get_steps()) {
    if (step && StringRef(step->tip).contains(tipSubstring)) {
      return true;
    }
  }
  return false;
}

const BugDiagStep *getLastStep(const BugReport *report) {
  if (!report || report->get_steps().empty()) {
    return nullptr;
  }
  return report->get_steps().back();
}

template <typename InstT>
InstT *findNthInstruction(Function *F, unsigned ordinal) {
  unsigned seen = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *inst = dyn_cast<InstT>(&I)) {
        if (seen == ordinal) {
          return inst;
        }
        ++seen;
      }
    }
  }
  return nullptr;
}

} // namespace

class PulseCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;

  void SetUp() override { DiagnosticManager::getInstance().clear(); }

  ExecutionDomain executeEntryBlock(PulseChecker &checker, Function *F,
                                    const Instruction *stop_after = nullptr) {
    ExecutionDomain state = checker.initializeFunction(F);
    for (auto &I : F->getEntryBlock()) {
      auto states =
          checker.executeInstruction(&I, std::move(state), nullptr, 0);
      EXPECT_FALSE(states.empty());
      if (states.empty()) {
        return ExecutionDomain();
      }
      state = std::move(states.front());
      if (&I == stop_after) {
        break;
      }
    }
    return state;
  }
};

#include "Fragments/PulseDiagnostics.inc"
#include "Fragments/PulseSummaries.inc"
#include "Fragments/PulseMemoryAndTaint.inc"
#include "Fragments/PulseAdvancedDiagnostics.inc"

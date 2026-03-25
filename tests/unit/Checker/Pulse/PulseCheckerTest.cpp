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
#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "Checker/Report/BugReportMgr.h"
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

CallInst *findCallByCalleeName(Function *F, StringRef calleeName) {
  for (auto &BB : *F) {
    for (auto &I : BB) {
      auto *call = dyn_cast<CallInst>(&I);
      if (!call || !call->getCalledFunction()) {
        continue;
      }
      if (call->getCalledFunction()->getName() == calleeName) {
        return call;
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

TEST_F(PulseCheckerTest, ReportsNullDereferenceWithConcreteWitness) {
  auto module = parseModule(context, R"(
    define i8 @null_deref() {
    entry:
      %v = load i8, i8* null
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("null_deref");
  ASSERT_NE(F, nullptr);
  LoadInst *sink = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(sink, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::NullDereference);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::NullDereference);
  ASSERT_NE(report, nullptr);
  ASSERT_TRUE(reportContainsTip(report, "Null pointer dereference"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, sink);
}

TEST_F(PulseCheckerTest, ReportsUninitializedReadAtLoadSite) {
  auto module = parseModule(context, R"(
    define i32 @uninit_read() {
    entry:
      %x = alloca i32
      %v = load i32, i32* %x
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("uninit_read");
  ASSERT_NE(F, nullptr);
  LoadInst *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::UninitializedRead);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Load from uninitialized variable"));
  EXPECT_TRUE(reportContainsTip(report, "Reading uninitialized memory"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, load);
}

TEST_F(PulseCheckerTest, ReportsInvalidFreeOfStackPointer) {
  auto module = parseModule(context, R"(
    declare void @free(i8*)

    define void @bad_free() {
    entry:
      %stack = alloca i8
      call void @free(i8* %stack)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("bad_free");
  ASSERT_NE(F, nullptr);
  CallInst *freeCall = findCallByCalleeName(F, "free");
  ASSERT_NE(freeCall, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::InvalidFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::InvalidFree);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, IssueType::InvalidFree);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Invalid free: non-heap pointer"));
  EXPECT_TRUE(reportContainsTip(report, "Invalid free of stack memory"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, freeCall);
}

TEST_F(PulseCheckerTest, ReportsStackAddressEscapeViaReturn) {
  auto module = parseModule(context, R"(
    define i8* @return_local() {
    entry:
      %x = alloca i8
      ret i8* %x
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("return_local");
  ASSERT_NE(F, nullptr);
  auto *ret = dyn_cast<ReturnInst>(F->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(
      mgr, IssueType::StackVariableAddressEscape);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter = getReportCountForType(
      mgr, IssueType::StackVariableAddressEscape);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::StackVariableAddressEscape);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report,
                                "Returning address derived from stack "
                                "allocation"));
  EXPECT_TRUE(reportContainsTip(report, "Stack address escapes via return"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, ret);
}

TEST_F(PulseCheckerTest, LatentIssuesAreNotReportedAtShutdown) {
  auto module = parseModule(context, R"(
    define void @latent_branch() {
    entry:
      %p = alloca i8
      %cmp = icmp eq i8* %p, null
      br i1 %cmp, label %bad, label %ok

    bad:
      ret void

    ok:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const int reportsBefore = mgr.get_total_reports();
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  EXPECT_EQ(mgr.get_total_reports(), reportsBefore);
}

TEST_F(PulseCheckerTest, UnlockDoesNotInvalidateLockMemory) {
  auto module = parseModule(context, R"(
    declare i32 @pthread_mutex_unlock(i8*)

    define void @unlock_ok() {
    entry:
      %m = alloca i8
      %r = call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("unlock_ok");
  ASSERT_NE(F, nullptr);

  auto *allocaInst = dyn_cast<AllocaInst>(&*F->getEntryBlock().begin());
  ASSERT_NE(allocaInst, nullptr);
  auto *unlockCall = dyn_cast<CallInst>(&*std::next(F->getEntryBlock().begin()));
  ASSERT_NE(unlockCall, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = executeEntryBlock(checker, F, unlockCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue lockAddr = checker.getFactory().getOrCreate(allocaInst);
  EXPECT_FALSE(astate->getPostAttrs().has(lockAddr, pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, NonReallocAssignmentDoesNotInvalidatePreviousValue) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define void @assign(i8** %slot) {
    entry:
      %new = call i8* @malloc(i64 8)
      store i8* %new, i8** %slot
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("assign");
  ASSERT_NE(F, nullptr);
  Argument *slotArg = F->arg_empty() ? nullptr : &*F->arg_begin();
  ASSERT_NE(slotArg, nullptr);

  auto callIt = F->getEntryBlock().begin();
  auto *mallocCall = dyn_cast<CallInst>(&*callIt);
  ASSERT_NE(mallocCall, nullptr);
  auto *store = dyn_cast<StoreInst>(&*std::next(callIt));
  ASSERT_NE(store, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue oldValue = checker.getFactory().createFresh();
  checker.getOperations().allocate(*astate, oldValue, nullptr);
  astate->getPostStack().add(slotArg, Address(oldValue));
  astate->getPostAttrs().remove(oldValue, pulse::Attribute::Invalid);
  astate->getPostAttrs().remove(oldValue, pulse::Attribute::Uninitialized);

  auto mallocStates =
      checker.executeInstruction(mallocCall, std::move(state), nullptr, 0);
  ASSERT_EQ(mallocStates.size(), 1u);
  state = std::move(mallocStates.front());

  auto storeStates =
      checker.executeInstruction(store, std::move(state), nullptr, 0);
  ASSERT_EQ(storeStates.size(), 1u);
  astate = storeStates.front().getAstate();
  ASSERT_NE(astate, nullptr);

  EXPECT_FALSE(
      astate->getPostAttrs().has(oldValue, pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, SummaryKeepsReturnValueFromMatchingExitPath) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @choose_ret(i8* %p) {
    entry:
      %isnull = icmp eq i8* %p, null
      br i1 %isnull, label %ret_null, label %ret_arg

    ret_null:
      ret i8* null

    ret_arg:
      ret i8* %p
    }

    define void @caller() {
    entry:
      %p = call i8* @malloc(i64 1)
      %q = call i8* @choose_ret(i8* %p)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  CallInst *mallocCall = findCallByCalleeName(caller, "malloc");
  CallInst *chooseCall = findCallByCalleeName(caller, "choose_ret");
  ASSERT_NE(mallocCall, nullptr);
  ASSERT_NE(chooseCall, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();

  ExecutionDomain state = executeEntryBlock(checker, caller, chooseCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  const Address *mallocAddr = astate->getPostStack().find(mallocCall);
  const Address *callAddr = astate->getPostStack().find(chooseCall);
  ASSERT_NE(mallocAddr, nullptr);
  ASSERT_NE(callAddr, nullptr);

  EXPECT_EQ(astate->getCanonical(mallocAddr->addr),
            astate->getCanonical(callAddr->addr));
  EXPECT_FALSE(
      astate->getPostAttrs().has(astate->getCanonical(callAddr->addr),
                                 pulse::Attribute::Null));
}

TEST_F(PulseCheckerTest, RejectedMultiEntrySummaryFallsBackToUnknownCall) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @pick_first(i8* %a, i8* %b, i1 %flag) {
    entry:
      %distinct = icmp ne i8* %a, %b
      br i1 %distinct, label %ok, label %reject

    ok:
      br i1 %flag, label %ret1, label %ret2

    ret1:
      ret i8* %a

    ret2:
      ret i8* %a

    reject:
      unreachable
    }

    define void @caller_same_ptr() {
    entry:
      %p = call i8* @malloc(i64 1)
      %q = call i8* @pick_first(i8* %p, i8* %p, i1 false)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller_same_ptr");
  ASSERT_NE(caller, nullptr);
  CallInst *pickCall = findCallByCalleeName(caller, "pick_first");
  ASSERT_NE(pickCall, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();

  ExecutionDomain state = executeEntryBlock(checker, caller, pickCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->hasSkippedCall("pick_first"));
}

TEST_F(PulseCheckerTest, LoopConvergenceRequiresEquivalentPathStamp) {
  auto module = parseModule(context, R"(
    define void @test_loop(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_loop");
  ASSERT_NE(F, nullptr);

  DominatorTree DT(*F);
  LoopInfo LI;
  LI.analyze(DT);

  LoopAbstraction loopAbs;
  loopAbs.initialize(LI);

  BasicBlock *header = nullptr;
  for (auto &BB : *F) {
    if (BB.getName() == "loop") {
      header = &BB;
      break;
    }
  }
  ASSERT_NE(header, nullptr);

  ExecutionDomain firstState;
  ExecutionDomain secondState;
  ExecutionDomain thirdState;

  AbstractValue a(nullptr, 1);
  AbstractValue b(nullptr, 2);
  ASSERT_TRUE(secondState.getAstate()->getPathFormula().addNull(a));
  ASSERT_TRUE(thirdState.getAstate()->getPathFormula().addNull(b));

  EXPECT_FALSE(loopAbs.visitHeader(header, firstState));
  EXPECT_TRUE(loopAbs.visitHeader(header, secondState));
  EXPECT_TRUE(loopAbs.visitHeader(header, thirdState));
  EXPECT_FALSE(loopAbs.hasPreviousIterationSamePathStamp(header));
}

TEST_F(PulseCheckerTest, ReallocAssignmentReportsUseAfterFree) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)

    define void @realloc_alias_uaf() {
    entry:
      %slot = alloca i8*
      %p0 = call i8* @malloc(i64 1)
      store i8* %p0, i8** %slot
      %alias = load i8*, i8** %slot
      %p1 = call i8* @realloc(i8* %p0, i64 2)
      store i8* %p1, i8** %slot
      %v = load i8, i8* %alias
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("realloc_alias_uaf");
  ASSERT_NE(F, nullptr);
  LoadInst *sink = findNthInstruction<LoadInst>(F, 1);
  ASSERT_NE(sink, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::UseAfterFree);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, IssueType::UseAfterFree);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Use after free detected"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, sink);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

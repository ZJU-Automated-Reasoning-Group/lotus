//===-- SifaDagInterpreterTest.cpp - Full Sifa DAG interpreter pipeline ----===//
//
// Exercises ProcedureResources -> DagInterpreter -> FixpointLoopSummarizer
// with ReachabilityDomain (same pipeline as isReachable()).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Caches/TopsortCache.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Fluid/AlwaysFluid.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Interpreter/IEnterCallRegistrar.h"
#include "Verification/Sifa/Procedure/ProcedureGraphBuilder.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/RegexDag/RegexDagUtils.h"
#include "Verification/Sifa/RegexDag/RegexToDag.h"
#include "Verification/Sifa/Sifa.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/Summarizers/ReUseSupersetCallSummarizer.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static const llvm::BasicBlock *getBlockByName(const llvm::Function &F, const char *name) {
  for (const llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
}

static const llvm::PHINode *getPhiByName(const llvm::BasicBlock &BB,
                                         const char *name) {
  for (const llvm::Instruction &I : BB) {
    const llvm::PHINode *phi = llvm::dyn_cast<llvm::PHINode>(&I);
    if (!phi) {
      break;
    }
    if (phi->getName() == name) {
      return phi;
    }
  }
  return nullptr;
}

static void expectOctagonPoint(const lotus::sifa::OctagonState &state,
                               const llvm::Value *value, int64_t point) {
  auto it = state.varToIndex().find(value);
  ASSERT_NE(it, state.varToIndex().end());
  const lotus::sifa::OctagonMatrix closed = state.matrix().strongClosure();
  auto lowerConstraint = closed.get(2 * it->second, 2 * it->second + 1);
  auto upperConstraint = closed.get(2 * it->second + 1, 2 * it->second);
  ASSERT_TRUE(lowerConstraint.hasValue());
  ASSERT_TRUE(upperConstraint.hasValue());
  EXPECT_EQ(-(*lowerConstraint) / 2, point);
  EXPECT_EQ((*upperConstraint) / 2, point);
}

class CountingEdgeDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  bool leq(const int &a, const int &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return false;
    return a <= b;
  }
  int join(const int &a, const int &b) const override {
    if (isBottom(a)) return b;
    if (isBottom(b)) return a;
    return std::max(a, b);
  }
  int widen(const int &previous, const int &next) const override {
    return join(previous, next);
  }
  int meet(const int &a, const int &b) const override {
    if (isBottom(a) || isBottom(b)) return bottom();
    return std::min(a, b);
  }
  int post(const lotus::sifa::Transition &t, const int &in) const override {
    if (isBottom(in)) return in;
    return t.kind == lotus::sifa::TransitionKind::Edge ? in + 1 : in;
  }
};

class AlteringSubsetDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  bool leq(const int &a, const int &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return false;
    return a <= b;
  }
  bool equal(const int &a, const int &b) const override { return a == b; }
  int join(const int &a, const int &b) const override { return std::max(a, b); }
  int widen(const int &previous, const int &next) const override {
    return join(previous, next);
  }
  int meet(const int &a, const int &b) const override { return std::min(a, b); }
  int post(const lotus::sifa::Transition &, const int &in) const override {
    return in;
  }
  SubsetEqResult subsetEq(const int &subset, const int &superset) const override {
    return SubsetEqResult(subset, superset + 1, true, true);
  }
};

class NoMeetSubsetDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  bool leq(const int &a, const int &b) const override {
    if (isBottom(a)) return true;
    if (isBottom(b)) return false;
    return a <= b;
  }
  bool equal(const int &a, const int &b) const override { return a == b; }
  int join(const int &a, const int &b) const override { return std::max(a, b); }
  int widen(const int &previous, const int &next) const override {
    return join(previous, next);
  }
  int post(const lotus::sifa::Transition &, const int &in) const override {
    return in;
  }
};

class AlphaDomain final
    : public lotus::sifa::AbstractDomain<lotus::sifa::Transition, int> {
public:
  int top() const override { return 0; }
  int bottom() const override { return -1; }
  bool isBottom(const int &s) const override { return s < 0; }
  int alpha(const int &) const override { return 100; }
  bool leq(const int &a, const int &b) const override { return a <= b; }
  int join(const int &a, const int &b) const override { return std::max(a, b); }
  int widen(const int &previous, const int &next) const override {
    return std::max(previous, next);
  }
  int post(const lotus::sifa::Transition &t, const int &in) const override {
    if (t.kind == lotus::sifa::TransitionKind::Edge) {
      return in + 1;
    }
    return in;
  }
};

class RecordingEnterCallRegistrar final
    : public lotus::sifa::IEnterCallRegistrar<int> {
public:
  void registerEnterCall(const std::string &calleeName,
                         const int &calleeInput) override {
    calls.emplace_back(calleeName, calleeInput);
  }

  std::vector<std::pair<std::string, int>> calls;
};

class CountingCallSummarizer final : public lotus::sifa::ICallSummarizer<int> {
public:
  int summarize(const std::string &, const int &) override { return ++calls; }

  int calls = 0;
};

TEST(SifaDagInterpreter, FullPipelineReachableAndUnreachable) {
  const char *ir = R"IR(
    define i32 @f(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      br label %loop

    exit:
      ret i32 0

    unreach:
      ret i32 1
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *body = getBlockByName(*F, "body");
  const llvm::BasicBlock *exit = getBlockByName(*F, "exit");
  const llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, bool> ipr(stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, bool> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *F,
      {const_cast<llvm::BasicBlock *>(body)});

  bool bodyReachable =
      ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(), /*initialInput=*/true);
  EXPECT_TRUE(bodyReachable);

  lotus::sifa::ProcedureResources resExit(stats, *F,
                                          {const_cast<llvm::BasicBlock *>(exit)});
  bool exitReachable =
      ipr.interpretForSingleMarker(resExit.getRegexDag(), resExit.getDagOverlayPathToLois(), true);
  EXPECT_TRUE(exitReachable);

  lotus::sifa::ProcedureResources resUnreach(stats, *F,
                                            {const_cast<llvm::BasicBlock *>(unreach)});
  bool unreachReachable =
      ipr.interpretForSingleMarker(resUnreach.getRegexDag(), resUnreach.getDagOverlayPathToLois(), true);
  EXPECT_FALSE(unreachReachable);
}

TEST(SifaDagInterpreter, RawProcedureGraphDoesNotAddCallSummaryEdges) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ProcedureGraphBuilder builder(stats, *F);
  lotus::sifa::ProcedureGraph graph = builder.graphOfProcedure({}, false);

  for (const auto &edge : graph.graph().getEdges()) {
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(edge->getLabel().kind, lotus::sifa::TransitionKind::Edge);
  }
}

TEST(SifaDagInterpreter, ReturnOverlayIgnoresNoReturnUnreachableExits) {
  const char *ir = R"IR(
    declare void @panic() noreturn

    define void @main() {
    entry:
      call void @panic()
      unreachable
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, bool> ipr(stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, bool> loopSum(
      stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(stats, *F, std::vector<llvm::BasicBlock *>{});
  bool reachesReturn = ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToReturn(), true);

  EXPECT_FALSE(reachesReturn);
}

TEST(SifaDagInterpreter, SingleSinkLocationReportsSyntheticExitAsNull) {
  const char *ir = R"IR(
    define void @main() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ProcedureResources res(stats, *F, std::vector<llvm::BasicBlock *>{});

  EXPECT_EQ(lotus::sifa::singleSinkLocation(
                res.getRegexDag(), res.getDagOverlayPathToReturn()),
            nullptr);
}

TEST(SifaDagInterpreter, TopsortCacheRefreshesAfterDagReuseAtSameAddress) {
  using Transition = lotus::sifa::Transition;
  using Regex = lotus::pathexpressions::Regex<Transition>;

  lotus::sifa::TopsortCache<Transition> cache;
  lotus::sifa::RegexToDag<Transition> toDag;
  auto a = Regex::literal(Transition::makeMarker(1, nullptr));
  auto b = Regex::literal(Transition::makeMarker(2, nullptr));

  toDag.add(a);
  lotus::sifa::RegexDag<Transition> dag = toDag.getDagAndReset();
  auto firstOrder = cache.topsort(dag);
  EXPECT_EQ(firstOrder.size(), 3u);

  toDag.add(Regex::concat(a, b));
  dag = toDag.getDagAndReset();
  auto secondOrder = cache.topsort(dag);
  EXPECT_EQ(secondOrder.size(), 4u);
}

TEST(SifaDagInterpreter, ProcedureResourcesIgnoreIrrelevantEnterCalls) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @h() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @g()
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *hFn = M->getFunction("h");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(hFn, nullptr);

  lotus::sifa::SifaStats stats;
  EXPECT_NO_THROW((void)lotus::sifa::ProcedureResources(
      stats, *mainFn, std::vector<llvm::BasicBlock *>{}, {hFn}));
}

TEST(SifaDagInterpreter, NativeDomainsApplyDestinationPhisAtBlockEntry) {
  const char *ir = R"IR(
    define i32 @f() {
    entry:
      br label %left

    left:
      br label %merge

    dead:
      br label %merge

    merge:
      %x = phi i32 [ 7, %left ], [ 42, %dead ]
      ret i32 %x
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *merge = getBlockByName(*F, "merge");
  const llvm::BasicBlock *left = getBlockByName(*F, "left");
  ASSERT_NE(merge, nullptr);
  ASSERT_NE(left, nullptr);

  const llvm::PHINode *phi = getPhiByName(*merge, "x");
  ASSERT_NE(phi, nullptr);

  lotus::sifa::IntervalState intervalState =
      lotus::sifa::analyzeToWithIntervalDomain(*F, *merge, lotus::sifa::IntervalState{});
  auto interval = intervalState.get(phi);
  ASSERT_TRUE(interval.hasValue());
  EXPECT_EQ(interval.getValue(), lotus::sifa::Interval::point(7));

  lotus::sifa::EqState eqState =
      lotus::sifa::analyzeToWithEqDomain(*F, *merge, lotus::sifa::EqState{});
  EXPECT_EQ(eqState.find(phi),
            eqState.find(phi->getIncomingValueForBlock(left)));

  lotus::sifa::ExplicitValueState explicitState =
      lotus::sifa::analyzeToWithExplicitValueDomain(
          *F, *merge, lotus::sifa::ExplicitValueState{});
  auto explicitValue = explicitState.get(phi);
  ASSERT_TRUE(explicitValue.hasValue());
  ASSERT_TRUE(explicitValue->value.hasValue());
  EXPECT_EQ(*explicitValue->value, 7);

  lotus::sifa::OctagonState octagonState =
      lotus::sifa::analyzeToWithOctagonDomain(*F, *merge, lotus::sifa::OctagonState{});
  EXPECT_EQ(octagonState.varToIndex().count(phi), 1u);
}

TEST(SifaDagInterpreter, NativeDomainsRefineTakenBranchGuards) {
  const char *ir = R"IR(
    define i32 @f() {
    entry:
      %cond = icmp eq i32 0, 0
      br i1 %cond, label %then, label %else

    then:
      br label %merge

    else:
      br label %merge

    merge:
      %x = phi i32 [ 7, %then ], [ 42, %else ]
      ret i32 %x
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *merge = getBlockByName(*F, "merge");
  const llvm::BasicBlock *thenBB = getBlockByName(*F, "then");
  ASSERT_NE(merge, nullptr);
  ASSERT_NE(thenBB, nullptr);

  const llvm::PHINode *phi = getPhiByName(*merge, "x");
  ASSERT_NE(phi, nullptr);

  lotus::sifa::IntervalState intervalState =
      lotus::sifa::analyzeToWithIntervalDomain(*F, *merge,
                                               lotus::sifa::IntervalState{});
  auto interval = intervalState.get(phi);
  ASSERT_TRUE(interval.hasValue());
  EXPECT_EQ(*interval, lotus::sifa::Interval::point(7));

  lotus::sifa::EqState eqState =
      lotus::sifa::analyzeToWithEqDomain(*F, *merge, lotus::sifa::EqState{});
  EXPECT_EQ(eqState.find(phi),
            eqState.find(phi->getIncomingValueForBlock(thenBB)));

  lotus::sifa::ExplicitValueState explicitState =
      lotus::sifa::analyzeToWithExplicitValueDomain(
          *F, *merge, lotus::sifa::ExplicitValueState{});
  auto explicitValue = explicitState.get(phi);
  ASSERT_TRUE(explicitValue.hasValue());
  ASSERT_TRUE(explicitValue->value.hasValue());
  EXPECT_EQ(*explicitValue->value, 7);

  lotus::sifa::OctagonState octagonState =
      lotus::sifa::analyzeToWithOctagonDomain(*F, *merge,
                                              lotus::sifa::OctagonState{});
  expectOctagonPoint(octagonState, phi, 7);
}

TEST(SifaDagInterpreter, EnterCallReceivesPrefixStateBeforeCallSite) {
  const char *ir = R"IR(
    define void @g() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      %x = add i32 1, 2
      call void @g()
      %y = add i32 %x, 1
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *gFn = M->getFunction("g");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(gFn, nullptr);

  lotus::sifa::SifaStats stats;
  CountingEdgeDomain domain;
  lotus::sifa::NeverFluid<int> fluid;
  lotus::sifa::DagInterpreter<lotus::sifa::Transition, int> ipr(stats, domain,
                                                                fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, int> loopSum(
      stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(stats, *mainFn, {}, {gFn});
  lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, int> storage;
  RecordingEnterCallRegistrar registrar;

  ipr.interpretWithCalls(res.getRegexDag(),
                         res.getDagOverlayPathToLoisAndEnterCalls(), 0,
                         storage, registrar);

  ASSERT_EQ(registrar.calls.size(), 1u);
  EXPECT_EQ(registrar.calls.front().first, "g");
  EXPECT_EQ(registrar.calls.front().second, 1);
}

TEST(SifaDagInterpreter, ReUseSupersetRequiresUnchangedSupersetInput) {
  AlteringSubsetDomain domain;
  CountingCallSummarizer inner;
  lotus::sifa::ReUseSupersetCallSummarizer<lotus::sifa::Transition, int>
      summarizer(domain, inner);

  EXPECT_EQ(summarizer.summarize("callee", 1), 1);
  EXPECT_EQ(inner.calls, 1);
  EXPECT_EQ(summarizer.summarize("callee", 1), 2);
  EXPECT_EQ(inner.calls, 2);
}

TEST(SifaDagInterpreter, ReUseSupersetFallsBackWhenDomainDoesNotSupportMeet) {
  NoMeetSubsetDomain domain;
  CountingCallSummarizer inner;
  lotus::sifa::ReUseSupersetCallSummarizer<lotus::sifa::Transition, int>
      summarizer(domain, inner);

  EXPECT_EQ(summarizer.summarize("callee", 2), 1);
  EXPECT_EQ(summarizer.summarize("callee", 3), 2);
  EXPECT_EQ(inner.calls, 2);

  EXPECT_EQ(summarizer.summarize("callee", 1), 3);
  EXPECT_EQ(inner.calls, 3);

  EXPECT_EQ(summarizer.summarize("callee", 2), 1);
  EXPECT_EQ(inner.calls, 3);
}

TEST(SifaDagInterpreter, ProcedureGraphBuilderEnterCallOverloadHonorsRestrictFlag) {
  const char *ir = R"IR(
    define void @callee() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @callee()
      ret void

    dead:
      unreachable
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *mainFn = M->getFunction("main");
  llvm::Function *calleeFn = M->getFunction("callee");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(calleeFn, nullptr);

  const llvm::BasicBlock *dead = getBlockByName(*mainFn, "dead");
  ASSERT_NE(dead, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ProcedureGraphBuilder builder(stats, *mainFn);

  lotus::sifa::ProcedureGraph full =
      builder.graphOfProcedure({}, {calleeFn}, /*restrictToReachable=*/false);
  EXPECT_NE(full.getBlockEntryNode(*dead), nullptr);

  lotus::sifa::ProcedureGraph restricted =
      builder.graphOfProcedure({}, {calleeFn}, /*restrictToReachable=*/true);
  EXPECT_EQ(restricted.getBlockEntryNode(*dead), nullptr);
}

TEST(SifaDagInterpreter, TransitionEqualityIncludesCfgIdentity) {
  const char *ir = R"IR(
    define void @f() {
    entry:
      br label %exit

    exit:
      ret void
    }

    define void @g() {
    entry:
      br label %exit

    exit:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *f = M->getFunction("f");
  llvm::Function *g = M->getFunction("g");
  ASSERT_NE(f, nullptr);
  ASSERT_NE(g, nullptr);

  auto *fEntry = &f->getEntryBlock();
  auto *gEntry = &g->getEntryBlock();
  auto *fExit = getBlockByName(*f, "exit");
  auto *gExit = getBlockByName(*g, "exit");
  ASSERT_NE(fExit, nullptr);
  ASSERT_NE(gExit, nullptr);

  const lotus::sifa::Transition a =
      lotus::sifa::Transition::makeEdge(0, fEntry, fExit);
  const lotus::sifa::Transition b =
      lotus::sifa::Transition::makeEdge(0, gEntry, gExit);

  EXPECT_FALSE(a == b);
}

TEST(SifaDagInterpreter, AnalyzeToCanUseCustomFluidPolicy) {
  const char *ir = R"IR(
    define void @main() {
    entry:
      br label %target

    target:
      ret void
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);
  const llvm::BasicBlock *target = getBlockByName(*F, "target");
  ASSERT_NE(target, nullptr);

  AlphaDomain domain;
  lotus::sifa::AlwaysFluid<int> fluid;

  EXPECT_EQ(lotus::sifa::analyzeTo<int>(*F, *target, 1, domain), 2);
  EXPECT_EQ(lotus::sifa::analyzeTo<int>(*F, *target, 1, domain, fluid), 100);
}

TEST(SifaDagInterpreter, AnalyzeToReturnPreservesBottomInitialState) {
  const char *ir = R"IR(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("main");
  ASSERT_NE(F, nullptr);

  lotus::sifa::IntervalState intervalBottom(true);
  lotus::sifa::IntervalState intervalResult =
      lotus::sifa::analyzeToReturnWithIntervalDomain(*F, intervalBottom);
  EXPECT_TRUE(intervalResult.isBottom());

  lotus::sifa::OctagonState octagonBottom(true);
  lotus::sifa::OctagonState octagonResult =
      lotus::sifa::analyzeToReturnWithOctagonDomain(*F, octagonBottom);
  EXPECT_TRUE(octagonResult.isBottom());
}

} // namespace

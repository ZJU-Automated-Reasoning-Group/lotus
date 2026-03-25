#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Sifa.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"
#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Utils/Config.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static llvm::BasicBlock *getBlockByName(llvm::Function &F, const char *name) {
  for (llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
}

static symabs_ai::configparser::Config makeSymAbsConfig() {
  symabs_ai::configparser::Config cfg;
  cfg.set("ModuleContext", "Recursive", true);
  cfg.set("Analyzer", "Variant", llvm::StringRef("UnilateralAnalyzer"));
  cfg.set("AbstractDomain", "Variant", llvm::StringRef("Interval"));
  cfg.set("FunctionContext", "RepresentAllInstructions", true);
  return cfg;
}

TEST(SifaSymAbs, SmokeIntervalsOctagonAndCalls) {
  const char *ir = R"IR(
    define i32 @g(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @f(i32 %n) {
    entry:
      %c = call i32 @g(i32 %n)
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %c
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

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  llvm::BasicBlock *body = getBlockByName(*F, "body");
  llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval, Octagon";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto bodyState = lotus::sifa::analyzeSymAbsTo(*M, *F, *body, opt);
  ASSERT_NE(bodyState, nullptr);
  EXPECT_FALSE(bodyState->isBottom());

  auto unreachState = lotus::sifa::analyzeSymAbsTo(*M, *F, *unreach, opt);
  EXPECT_TRUE(unreachState == nullptr || unreachState->isBottom());

  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, SingleBlockFunctionReturnStateNotBottom) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, AnalyzeSymAbsToSpecificBlock) {
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

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  llvm::BasicBlock *body = getBlockByName(*F, "body");
  llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval, Octagon";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  auto stateBody = lotus::sifa::analyzeSymAbsTo(*M, *F, *body, opt);
  auto stateUnreach = lotus::sifa::analyzeSymAbsTo(*M, *F, *unreach, opt);

  EXPECT_NE(stateBody, nullptr);
  EXPECT_FALSE(stateBody->isBottom());
  EXPECT_TRUE(stateUnreach == nullptr || stateUnreach->isBottom());
}

TEST(SifaSymAbs, IntervalOnlyDomain) {
  const char *ir = R"IR(
    define i32 @f(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  lotus::sifa::SifaSymAbsOptions opt;
  opt.abstractDomain = "Interval";
  opt.analyzerVariant = "UnilateralAnalyzer";
  opt.recursive = true;

  EXPECT_TRUE(lotus::sifa::isReachableSymAbs(*M, *F, F->getEntryBlock(), opt));
  auto retState = lotus::sifa::analyzeSymAbsToReturn(*M, *F, opt);
  ASSERT_NE(retState, nullptr);
  EXPECT_FALSE(retState->isBottom());
}

TEST(SifaSymAbs, HybridFallbackHandlesSplitCallTransitions) {
  const char *ir = R"IR(
    define i32 @callee(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @caller(i32 %n) {
    entry:
      %a = add i32 %n, 2
      %b = call i32 @callee(i32 %a)
      %c = add i32 %b, 3
      br label %exit

    exit:
      ret i32 %c
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *caller = M->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  llvm::BasicBlock *exit = getBlockByName(*caller, "exit");
  ASSERT_NE(exit, nullptr);

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto fctx = mctx.createFunctionContext(caller);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctx);
  symabs_ai::DomainConstructor dom(fctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*fctx, fragDecomp, dom);

  lotus::sifa::SifaStats stats;
  lotus::sifa::SifaSymAbsDomain domain(*fctx, dom, *analyzer);
  lotus::sifa::NeverFluid<lotus::sifa::SymAbsState> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, lotus::sifa::SymAbsState> ipr(
      stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, lotus::sifa::SymAbsState>
      loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *caller, {exit}, std::vector<const llvm::Function *>{});
  auto initial = domain.makeTopAt(&caller->getEntryBlock(), /*after=*/false);

  lotus::sifa::SymAbsState out;
  EXPECT_NO_THROW(
      out = ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(),
                                         initial));
  ASSERT_NE(out, nullptr);
  EXPECT_FALSE(out->isBottom());
}

TEST(SifaSymAbs, HybridFallbackHandlesInvokeReturnSummary) {
  const char *ir = R"IR(
    declare i32 @__gxx_personality_v0()

    define i32 @callee(i32 %x) {
    entry:
      %y = add i32 %x, 5
      ret i32 %y
    }

    define i32 @caller(i32 %n) personality i32 ()* @__gxx_personality_v0 {
    entry:
      %res = invoke i32 @callee(i32 %n)
        to label %cont unwind label %lpad

    cont:
      %sum = add i32 %res, 1
      ret i32 %sum

    lpad:
      %ex = landingpad i32 cleanup
      ret i32 0
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  llvm::Function *caller = M->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  llvm::BasicBlock *cont = getBlockByName(*caller, "cont");
  ASSERT_NE(cont, nullptr);

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto fctx = mctx.createFunctionContext(caller);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctx);
  symabs_ai::DomainConstructor dom(fctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*fctx, fragDecomp, dom);

  lotus::sifa::SifaStats stats;
  lotus::sifa::SifaSymAbsDomain domain(*fctx, dom, *analyzer);
  lotus::sifa::NeverFluid<lotus::sifa::SymAbsState> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, lotus::sifa::SymAbsState> ipr(
      stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, lotus::sifa::SymAbsState>
      loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *caller, {cont}, std::vector<const llvm::Function *>{});
  auto initial = domain.makeTopAt(&caller->getEntryBlock(), /*after=*/false);

  lotus::sifa::SymAbsState out;
  EXPECT_NO_THROW(
      out = ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(),
                                         initial));
  ASSERT_NE(out, nullptr);
  EXPECT_FALSE(out->isBottom());
}

TEST(SifaSymAbs, InterproceduralEnterCallProjectsArguments) {
  const char *ir = R"IR(
    define void @callee(i32 %x) {
    entry:
      %cmp = icmp eq i32 %x, 42
      br i1 %cmp, label %good, label %bad

    good:
      ret void

    bad:
      ret void
    }

    define void @main(i32 %seed) {
    entry:
      %unused = add i32 %seed, 0
      call void @callee(i32 42)
      ret void
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

  llvm::BasicBlock *good = getBlockByName(*calleeFn, "good");
  llvm::BasicBlock *bad = getBlockByName(*calleeFn, "bad");
  ASSERT_NE(good, nullptr);
  ASSERT_NE(bad, nullptr);

  symabs_ai::ModuleContext mctx(M.get(), makeSymAbsConfig());
  auto rootFctx = mctx.createFunctionContext(mainFn);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*rootFctx);
  symabs_ai::DomainConstructor dom(rootFctx->getConfig());
  auto analyzer = symabs_ai::Analyzer::New(*rootFctx, fragDecomp, dom);

  lotus::sifa::SifaSymAbsDomain domain(*rootFctx, dom, *analyzer);

  auto goodState = lotus::sifa::analyzeInterproceduralTo<lotus::sifa::SymAbsState>(
      *M, mainFn, *calleeFn, *good, domain.top(), domain);
  ASSERT_NE(goodState, nullptr);
  EXPECT_FALSE(goodState->isBottom());

  auto badState = lotus::sifa::analyzeInterproceduralTo<lotus::sifa::SymAbsState>(
      *M, mainFn, *calleeFn, *bad, domain.top(), domain);
  EXPECT_TRUE(!badState || badState->isBottom());
}

} // namespace

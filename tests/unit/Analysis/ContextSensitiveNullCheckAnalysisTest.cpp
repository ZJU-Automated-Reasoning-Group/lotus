#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <gtest/gtest.h>

using namespace lotus::unittest;

namespace lotus {
namespace nullpointer {
namespace testing {
void setContextSensitiveNullContextDepthOverrideForTesting(int Depth);
} // namespace testing
} // namespace nullpointer
} // namespace lotus

namespace {

using Context = ContextSensitiveNullCheckAnalysis::Context;

bool contextEquals(const Context &Ctx,
                   std::initializer_list<llvm::CallBase *> Expected) {
  const auto &Elements = Ctx.elements();
  if (Elements.size() != Expected.size()) {
    return false;
  }
  auto It = Elements.begin();
  for (auto *Call : Expected) {
    if (*It++ != Call) {
      return false;
    }
  }
  return true;
}

struct AnalysisHarness {
  std::unique_ptr<llvm::legacy::PassManager> PassManager;
  ContextSensitiveNullCheckAnalysis *Analysis = nullptr;
};

class ScopedContextDepthOverride {
public:
  explicit ScopedContextDepthOverride(int Depth) {
    lotus::nullpointer::testing::
        setContextSensitiveNullContextDepthOverrideForTesting(Depth);
  }

  ~ScopedContextDepthOverride() {
    lotus::nullpointer::testing::
        setContextSensitiveNullContextDepthOverrideForTesting(-1);
  }
};

AnalysisHarness runContextSensitiveNCA(llvm::Module &Module) {
  AnalysisHarness Harness;
  Harness.PassManager = std::make_unique<llvm::legacy::PassManager>();
  Harness.PassManager->add(new DyckAliasAnalysis());
  Harness.PassManager->add(new DyckModRefAnalysis());
  Harness.Analysis = new ContextSensitiveNullCheckAnalysis();
  Harness.PassManager->add(Harness.Analysis);
  Harness.PassManager->run(Module);
  return Harness;
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     DistinguishesExactCallerContextsButKeepsWholeFunctionQueriesConservative) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @caller_nonnull() {
    entry:
      %stack = alloca i8, align 1
      call void @callee(i8* %stack)
      ret void
    }

    define void @caller_nullable(i8* %arg) {
    entry:
      call void @callee(i8* %arg)
      ret void
    }

    define i32 @main() {
    entry:
      call void @caller_nonnull()
      call void @caller_nullable(i8* null)
      ret i32 0
    }
  )", "ContextSensitiveNullCheckAnalysisTest");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *NonNullCaller = Module->getFunction("caller_nonnull");
  auto *NullableCaller = Module->getFunction("caller_nullable");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(NonNullCaller, nullptr);
  ASSERT_NE(NullableCaller, nullptr);
  ASSERT_NE(Load, nullptr);

  auto *Formal = Callee->getArg(0);
  auto MainToNonNull = findCallsTo(Module->getFunction("main"), "caller_nonnull");
  auto MainToNullable =
      findCallsTo(Module->getFunction("main"), "caller_nullable");
  auto NonNullToCallee = findCallsTo(NonNullCaller, "callee");
  auto NullableToCallee = findCallsTo(NullableCaller, "callee");
  ASSERT_EQ(MainToNonNull.size(), 1u);
  ASSERT_EQ(MainToNullable.size(), 1u);
  ASSERT_EQ(NonNullToCallee.size(), 1u);
  ASSERT_EQ(NullableToCallee.size(), 1u);

  auto Reachable = Analysis->getReachableContexts(Load);
  ASSERT_GE(Reachable.size(), 2u);

  const Context *ExactNonNullCtx = nullptr;
  const Context *ExactNullableCtx = nullptr;
  for (const auto &Ctx : Reachable) {
    if (contextEquals(Ctx, {MainToNonNull.front(), NonNullToCallee.front()})) {
      ExactNonNullCtx = &Ctx;
    }
    if (contextEquals(Ctx, {MainToNullable.front(), NullableToCallee.front()})) {
      ExactNullableCtx = &Ctx;
    }
  }

  ASSERT_NE(ExactNonNullCtx, nullptr);
  ASSERT_NE(ExactNullableCtx, nullptr);
  EXPECT_FALSE(Analysis->mayNull(Formal, Load, *ExactNonNullCtx));
  EXPECT_TRUE(Analysis->mayNull(Formal, Load, *ExactNullableCtx));
  EXPECT_TRUE(Analysis->mayNull(Formal, Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     ResolvesIndirectCallsAndTracksMultipleContextsConservatively) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @dispatch(void (i8*)* %fp, i8* %p) {
    entry:
      call void %fp(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      call void @dispatch(void (i8*)* @callee, i8* %stack)
      call void @dispatch(void (i8*)* @callee, i8* null)
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *Dispatch = Module->getFunction("dispatch");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(Dispatch, nullptr);
  ASSERT_NE(Load, nullptr);

  llvm::CallBase *IndirectCall = nullptr;
  for (auto &BB : *Dispatch) {
    for (auto &Inst : BB) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (Call && !Call->getCalledFunction()) {
        IndirectCall = Call;
      }
    }
  }
  ASSERT_NE(IndirectCall, nullptr);

  auto MainToDispatch = findCallsTo(Module->getFunction("main"), "dispatch");
  ASSERT_EQ(MainToDispatch.size(), 2u);

  auto Reachable = Analysis->getReachableContexts(Load);
  EXPECT_GE(Reachable.size(), 2u);
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load));

  const Context *ExactNonNullCtx = nullptr;
  const Context *ExactNullableCtx = nullptr;
  for (const auto &Ctx : Reachable) {
    if (contextEquals(Ctx, {MainToDispatch[0], IndirectCall})) {
      ExactNonNullCtx = &Ctx;
    }
    if (contextEquals(Ctx, {MainToDispatch[1], IndirectCall})) {
      ExactNullableCtx = &Ctx;
    }
  }

  ASSERT_NE(ExactNonNullCtx, nullptr);
  ASSERT_NE(ExactNullableCtx, nullptr);
  EXPECT_FALSE(Analysis->mayNull(Callee->getArg(0), Load, *ExactNonNullCtx));
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load, *ExactNullableCtx));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     KZeroReturnPropagationKeepsSingleCallerResults) {
  ScopedContextDepthOverride DepthOverride(0);

  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @identity(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %ret = call i8* @identity(i8* %stack)
      %load = load i8, i8* %ret, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *Ret = findInstructionByName(Main, "ret");
  auto *Load = findInstructionByName(Main, "load");
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Load, nullptr);

  EXPECT_FALSE(Analysis->mayNull(Ret, Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     KZeroMergesReturnFactsAcrossCallersConservatively) {
  ScopedContextDepthOverride DepthOverride(0);

  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @identity(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %nonnull = call i8* @identity(i8* %stack)
      %nullable = call i8* @identity(i8* null)
      %load_nonnull = load i8, i8* %nonnull, align 1
      %load_nullable = load i8, i8* %nullable, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *NonNullRet = findInstructionByName(Main, "nonnull");
  auto *NullableRet = findInstructionByName(Main, "nullable");
  auto *LoadNonNull = findInstructionByName(Main, "load_nonnull");
  auto *LoadNullable = findInstructionByName(Main, "load_nullable");
  ASSERT_NE(NonNullRet, nullptr);
  ASSERT_NE(NullableRet, nullptr);
  ASSERT_NE(LoadNonNull, nullptr);
  ASSERT_NE(LoadNullable, nullptr);

  EXPECT_TRUE(Analysis->mayNull(NonNullRet, LoadNonNull));
  EXPECT_TRUE(Analysis->mayNull(NullableRet, LoadNullable));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     KLimitingDoesNotProduceFalseNotNullWhenOlderPrefixesDiffer) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @leaf_nonnull(i8* %p) {
    entry:
      call void @callee(i8* %p)
      ret void
    }

    define void @mid_a(i8* %p) {
    entry:
      call void @leaf_nonnull(i8* %p)
      ret void
    }

    define void @mid_b(i8* %p) {
    entry:
      call void @mid_a(i8* %p)
      ret void
    }

    define void @root_nonnull() {
    entry:
      %stack = alloca i8, align 1
      call void @mid_b(i8* %stack)
      ret void
    }

    define void @root_nullable() {
    entry:
      call void @mid_b(i8* null)
      ret void
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(Load, nullptr);

  auto Reachable = Analysis->getReachableContexts(Load);
  ASSERT_FALSE(Reachable.empty());
  bool SawCollapsedMayNull = false;
  for (const auto &Ctx : Reachable) {
    if (Ctx.size() == 3 && Analysis->mayNull(Callee->getArg(0), Load, Ctx)) {
      SawCollapsedMayNull = true;
    }
  }
  EXPECT_TRUE(SawCollapsedMayNull);
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     KOneCollapsesSameSuffixContextsConservatively) {
  ScopedContextDepthOverride DepthOverride(1);

  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define void @callee(i8* %p) {
    entry:
      %load = load i8, i8* %p, align 1
      ret void
    }

    define void @dispatch(i8* %p) {
    entry:
      call void @callee(i8* %p)
      ret void
    }

    define void @root_nonnull() {
    entry:
      %stack = alloca i8, align 1
      call void @dispatch(i8* %stack)
      ret void
    }

    define void @root_nullable() {
    entry:
      call void @dispatch(i8* null)
      ret void
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Callee = Module->getFunction("callee");
  auto *Dispatch = Module->getFunction("dispatch");
  auto *Load = findInstructionByName(Callee, "load");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Callee, nullptr);
  ASSERT_NE(Dispatch, nullptr);
  ASSERT_NE(Load, nullptr);

  auto DispatchToCallee = findCallsTo(Dispatch, "callee");
  ASSERT_EQ(DispatchToCallee.size(), 1u);
  auto *DispatchCall = DispatchToCallee.front();

  auto Reachable = Analysis->getReachableContexts(Load);
  ASSERT_EQ(Reachable.size(), 1u);
  auto DispatchReachable = Analysis->getReachableContexts(DispatchCall);
  EXPECT_EQ(DispatchReachable.size(), 2u);
  EXPECT_TRUE(contextEquals(Reachable.front(), {DispatchToCallee.front()}));
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load, Reachable.front()));
  EXPECT_TRUE(Analysis->mayNull(Callee->getArg(0), Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     KOneRestoresCallerContextsAfterTruncatedNestedReturn) {
  ScopedContextDepthOverride DepthOverride(1);

  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @leaf(i8* %p) {
    entry:
      ret i8* %p
    }

    define void @middle(i8* %p) {
    entry:
      %retptr = call i8* @leaf(i8* %p)
      %load = load i8, i8* %retptr, align 1
      ret void
    }

    define void @outer_nonnull() {
    entry:
      %stack = alloca i8, align 1
      call void @middle(i8* %stack)
      ret void
    }

    define void @outer_nullable(i8* %arg) {
    entry:
      call void @middle(i8* %arg)
      ret void
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Leaf = Module->getFunction("leaf");
  auto *Middle = Module->getFunction("middle");
  auto *OuterNonNull = Module->getFunction("outer_nonnull");
  auto *OuterNullable = Module->getFunction("outer_nullable");
  auto *Load = findInstructionByName(Middle, "load");
  auto *LeafRet = Leaf ? Leaf->getEntryBlock().getTerminator() : nullptr;
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Leaf, nullptr);
  ASSERT_NE(Middle, nullptr);
  ASSERT_NE(OuterNonNull, nullptr);
  ASSERT_NE(OuterNullable, nullptr);
  ASSERT_NE(Load, nullptr);
  ASSERT_NE(LeafRet, nullptr);

  auto OuterNonNullToMiddle = findCallsTo(OuterNonNull, "middle");
  auto OuterNullableToMiddle = findCallsTo(OuterNullable, "middle");
  auto MiddleToLeaf = findCallsTo(Middle, "leaf");
  ASSERT_EQ(OuterNonNullToMiddle.size(), 1u);
  ASSERT_EQ(OuterNullableToMiddle.size(), 1u);
  ASSERT_EQ(MiddleToLeaf.size(), 1u);

  auto LeafReachable = Analysis->getReachableContexts(LeafRet);
  ASSERT_EQ(LeafReachable.size(), 1u);
  EXPECT_TRUE(contextEquals(LeafReachable.front(), {MiddleToLeaf.front()}));

  auto Reachable = Analysis->getReachableContexts(Load);
  ASSERT_EQ(Reachable.size(), 2u);

  const Context *NonNullCtx = nullptr;
  const Context *NullableCtx = nullptr;
  for (const auto &Ctx : Reachable) {
    if (contextEquals(Ctx, {OuterNonNullToMiddle.front()})) {
      NonNullCtx = &Ctx;
    }
    if (contextEquals(Ctx, {OuterNullableToMiddle.front()})) {
      NullableCtx = &Ctx;
    }
  }

  ASSERT_NE(NonNullCtx, nullptr);
  ASSERT_NE(NullableCtx, nullptr);
  EXPECT_FALSE(Analysis->mayNull(Middle->getArg(0), Load, *NonNullCtx));
  EXPECT_TRUE(Analysis->mayNull(Middle->getArg(0), Load, *NullableCtx));
  EXPECT_TRUE(Analysis->mayNull(Middle->getArg(0), Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     MatchesReturnedNonNullFactsToTheCorrectCallSite) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @identity(i8* %p) {
    entry:
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %nonnull = call i8* @identity(i8* %stack)
      %nullable = call i8* @identity(i8* null)
      %load_nonnull = load i8, i8* %nonnull, align 1
      %load_nullable = load i8, i8* %nullable, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *NonNullRet = findInstructionByName(Main, "nonnull");
  auto *NullableRet = findInstructionByName(Main, "nullable");
  auto *LoadNonNull = findInstructionByName(Main, "load_nonnull");
  auto *LoadNullable = findInstructionByName(Main, "load_nullable");
  ASSERT_NE(NonNullRet, nullptr);
  ASSERT_NE(NullableRet, nullptr);
  ASSERT_NE(LoadNonNull, nullptr);
  ASSERT_NE(LoadNullable, nullptr);

  EXPECT_FALSE(Analysis->mayNull(NonNullRet, LoadNonNull));
  EXPECT_TRUE(Analysis->mayNull(NullableRet, LoadNullable));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     RecursiveReturnPropagationReachesAFixpoint) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @recur(i8* %p, i1 %stop) {
    entry:
      br i1 %stop, label %base, label %step
    base:
      ret i8* %p
    step:
      %next = call i8* @recur(i8* %p, i1 true)
      ret i8* %next
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %ret = call i8* @recur(i8* %stack, i1 false)
      %load = load i8, i8* %ret, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *Ret = findInstructionByName(Main, "ret");
  auto *Load = findInstructionByName(Main, "load");
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Load, nullptr);

  EXPECT_FALSE(Analysis->mayNull(Ret, Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     RecursiveReturnPropagationStaysSoundUnderKOneTruncation) {
  ScopedContextDepthOverride DepthOverride(1);

  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    define i8* @recur(i8* %p, i1 %stop) {
    entry:
      br i1 %stop, label %base, label %step
    base:
      ret i8* %p
    step:
      %next = call i8* @recur(i8* %p, i1 true)
      ret i8* %next
    }

    define i32 @main() {
    entry:
      %stack = alloca i8, align 1
      %ret = call i8* @recur(i8* %stack, i1 false)
      %load = load i8, i8* %ret, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *Ret = findInstructionByName(Main, "ret");
  auto *Load = findInstructionByName(Main, "load");
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Load, nullptr);

  EXPECT_FALSE(Analysis->mayNull(Ret, Load));
}

TEST(ContextSensitiveNullCheckAnalysisTest,
     OnlyStackAllocasAndNonnullContractsSeedGuaranteedNonNullFacts) {
  llvm::LLVMContext ContextStorage;
  auto Module = parseModule(ContextStorage, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      %heap = call i8* @malloc(i64 4)
      %stack = alloca i8, align 1
      %heap_load = load i8, i8* %heap, align 1
      %stack_load = load i8, i8* %stack, align 1
      ret i32 0
    }
  )");
  ASSERT_NE(Module, nullptr);

  auto Harness = runContextSensitiveNCA(*Module);
  auto *Analysis = Harness.Analysis;
  auto *Main = Module->getFunction("main");
  ASSERT_NE(Analysis, nullptr);
  ASSERT_NE(Main, nullptr);

  auto *Heap = findInstructionByName(Main, "heap");
  auto *Stack = findInstructionByName(Main, "stack");
  auto *HeapLoad = findInstructionByName(Main, "heap_load");
  auto *StackLoad = findInstructionByName(Main, "stack_load");
  ASSERT_NE(Heap, nullptr);
  ASSERT_NE(Stack, nullptr);
  ASSERT_NE(HeapLoad, nullptr);
  ASSERT_NE(StackLoad, nullptr);

  EXPECT_TRUE(Analysis->mayNull(Heap, HeapLoad));
  EXPECT_FALSE(Analysis->mayNull(Stack, StackLoad));
}

} // namespace

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/NPA/Analyses/Inter/Nullability.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findBlock;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

} // namespace

TEST(NPA, InterproceduralNullabilityTracksNullStoreAndLoad) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot, align 8
      %p = load i8*, i8** %slot, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, p));
  EXPECT_TRUE(result.isMaybeNullMemory(after, slot));
}

TEST(NPA, InterproceduralNullabilityUsesBlockExitFactsForQueries) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main() {
    entry:
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot, align 8
      %p = load i8*, i8** %slot, align 8
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *entry = findBlock(*mainFn, "entry");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(slot, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNull(entry, p));
  EXPECT_TRUE(result.isMaybeNullMemory(entry, slot));
}

TEST(NPA, InterproceduralNullabilityTreatsAllocaPointersAsNonNull) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main() {
    entry:
      %p = alloca i8, align 1
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_FALSE(result.isMaybeNull(after, p));
}

TEST(NPA, InterproceduralNullabilityPropagatesThroughPhi) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main(i1 %cond) {
    entry:
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      br i1 %cond, label %t, label %f

    t:
      br label %merge

    f:
      br label %merge

    merge:
      %p = phi i8* [ %nonnull, %t ], [ null, %f ]
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, p));
}

TEST(NPA, InterproceduralNullabilityPropagatesNullThroughReturn) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i8* @id(i8* %x) {
    entry:
      ret i8* %x
    }

    define i32 @main() {
    entry:
      %p = call i8* @id(i8* null)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, p));
}

TEST(NPA, InterproceduralNullabilityPreservesConstantNullReturnBranches) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i8* @maybe_null(i1 %cond, i8* %x) {
    entry:
      br i1 %cond, label %ret_null, label %ret_arg

    ret_null:
      ret i8* null

    ret_arg:
      ret i8* %x
    }

    define i32 @main(i1 %cond) {
    entry:
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %p = call i8* @maybe_null(i1 %cond, i8* %nonnull)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, p));
}

TEST(NPA, InterproceduralNullabilityTracksCalleePointerStoreNullWrites) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @clear_slot(i8** %slot) {
    entry:
      store i8* null, i8** %slot, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* %nonnull, i8** %slot, align 8
      call void @clear_slot(i8** %slot)
      %p = load i8*, i8** %slot, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNullMemory(after, slot));
  EXPECT_TRUE(result.isMaybeNull(after, p));
}

TEST(NPA,
     InterproceduralNullabilityTreatsUnknownExternalPointerReturnsAsMaybeNull) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @ext()

    define i32 @main() {
    entry:
      %p = call i8* @ext()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, p));
}

TEST(NPA, InterproceduralNullabilitySeparatesSiblingFields) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    %pair = type { i8*, i8* }

    define i32 @main() {
    entry:
      %pair = alloca %pair, align 8
      %a = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 0
      %b = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 1
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      store i8* %nonnull, i8** %b, align 8
      store i8* null, i8** %a, align 8
      %x = load i8*, i8** %b, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *a = findInstructionByName(*mainFn, "a");
  const auto *b = findInstructionByName(*mainFn, "b");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNullMemory(after, a));
  EXPECT_FALSE(result.isMaybeNullMemory(after, b));
  EXPECT_FALSE(result.isMaybeNull(after, x));
}

TEST(NPA, InterproceduralNullabilitySeedsPointerFieldsInGlobalAggregates) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    %pair = type { i8*, i8* }
    @glob = global %pair { i8* null, i8* null }, align 8

    define i32 @main() {
    entry:
      %a = getelementptr inbounds %pair, %pair* @glob, i32 0, i32 0
      %x = load i8*, i8** %a, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *a = findInstructionByName(*mainFn, "a");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNullMemory(after, a));
  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA, InterproceduralNullabilityMapsPartitionedFieldsAcrossCalls) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    %pair = type { i8*, i8* }

    define i8* @get_second(%pair* %p) {
    entry:
      %b = getelementptr inbounds %pair, %pair* %p, i32 0, i32 1
      %x = load i8*, i8** %b, align 8
      ret i8* %x
    }

    define i32 @main() {
    entry:
      %pair = alloca %pair, align 8
      %a = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 0
      %b = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 1
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      store i8* null, i8** %a, align 8
      store i8* %nonnull, i8** %b, align 8
      %ret = call i8* @get_second(%pair* %pair)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *ret = findInstructionByName(*mainFn, "ret");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(ret, nullptr);

  EXPECT_FALSE(result.isMaybeNull(after, ret));
}

TEST(NPA, InterproceduralNullabilityUsesWeakUpdatesForMergedUnknownOffsets) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main(i64 %i, i64 %j) {
    entry:
      %arr = alloca [4 x i8*], align 8
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %pi = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 %i
      %pj = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 %j
      store i8* null, i8** %pi, align 8
      store i8* %nonnull, i8** %pj, align 8
      %x = load i8*, i8** %pi, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA, InterproceduralNullabilityUsesWeakUpdatesForMergedPointerCells) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    %pair = type { i8*, i8* }

    define i32 @main(i1 %cond) {
    entry:
      %pair = alloca %pair, align 8
      %a = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 0
      %b = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 1
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      store i8* %nonnull, i8** %a, align 8
      store i8* %nonnull, i8** %b, align 8
      br i1 %cond, label %t, label %f

    t:
      br label %merge

    f:
      br label %merge

    merge:
      %slot = phi i8** [ %a, %t ], [ %b, %f ]
      store i8* null, i8** %slot, align 8
      %x = load i8*, i8** %b, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *b = findInstructionByName(*mainFn, "b");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNullMemory(after, b));
  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA,
     InterproceduralNullabilityPropagatesImpreciseFormalMemoryAcrossCalls) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @clear_at(i8** %base, i64 %idx) {
    entry:
      %slot = getelementptr inbounds i8*, i8** %base, i64 %idx
      store i8* null, i8** %slot, align 8
      ret void
    }

    define i32 @main(i64 %idx) {
    entry:
      %arr = alloca [4 x i8*], align 8
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 %idx
      %base = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 0
      store i8* %nonnull, i8** %slot, align 8
      call void @clear_at(i8** %base, i64 %idx)
      %x = load i8*, i8** %slot, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA,
     InterproceduralNullabilityMapsExactOffsetsBackToImpreciseCallerCells) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @clear_next(i8** %base) {
    entry:
      %slot = getelementptr inbounds i8*, i8** %base, i64 1
      store i8* null, i8** %slot, align 8
      ret void
    }

    define i32 @main(i64 %idx) {
    entry:
      %arr = alloca [8 x i8*], align 8
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %base = getelementptr inbounds [8 x i8*], [8 x i8*]* %arr, i64 0, i64 %idx
      %next_idx = add i64 %idx, 1
      %next = getelementptr inbounds [8 x i8*], [8 x i8*]* %arr, i64 0, i64 %next_idx
      store i8* %nonnull, i8** %next, align 8
      call void @clear_next(i8** %base)
      %x = load i8*, i8** %next, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA, InterproceduralNullabilityClobbersReachableFieldsAtExternalCalls) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    %pair = type { i8*, i8* }

    declare void @ext_write(%pair*)

    define i32 @main() {
    entry:
      %pair = alloca %pair, align 8
      %a = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 0
      %b = getelementptr inbounds %pair, %pair* %pair, i32 0, i32 1
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      store i8* %nonnull, i8** %a, align 8
      store i8* %nonnull, i8** %b, align 8
      call void @ext_write(%pair* %pair)
      %x = load i8*, i8** %b, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *b = findInstructionByName(*mainFn, "b");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNullMemory(after, b));
  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA, InterproceduralNullabilityMapsNonZeroFieldWritesFromAliasingActuals) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    %pair = type { i8*, i8* }

    define void @clear_second(%pair* %p) {
    entry:
      %b = getelementptr inbounds %pair, %pair* %p, i32 0, i32 1
      store i8* null, i8** %b, align 8
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      %pair1 = alloca %pair, align 8
      %pair2 = alloca %pair, align 8
      %b1 = getelementptr inbounds %pair, %pair* %pair1, i32 0, i32 1
      %b2 = getelementptr inbounds %pair, %pair* %pair2, i32 0, i32 1
      %buf = alloca [8 x i8], align 1
      %nonnull = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      store i8* %nonnull, i8** %b1, align 8
      store i8* %nonnull, i8** %b2, align 8
      br i1 %cond, label %t, label %f

    t:
      br label %merge

    f:
      br label %merge

    merge:
      %alias = phi %pair* [ %pair1, %t ], [ %pair2, %f ]
      call void @clear_second(%pair* %alias)
      %x = load i8*, i8** %b1, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  auto result = npa::InterNullability::run(*module);
  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *b1 = findInstructionByName(*mainFn, "b1");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(b1, nullptr);
  ASSERT_NE(x, nullptr);

  EXPECT_TRUE(result.isMaybeNullMemory(after, b1));
  EXPECT_TRUE(result.isMaybeNull(after, x));
}

TEST(NPA, InterproceduralNullabilityPropagatesAcrossMayAliasMemory) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main(i8** %a, i8** %b) {
    entry:
      store i8* null, i8** %a, align 8
      %x = load i8*, i8** %b, align 8
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterNullability::Options options;
  options.seed_entry_pointer_args = false;
  auto result = npa::InterNullability::run(*module, wrapper, options);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  const llvm::Value *b = mainFn->getArg(1);
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);
  ASSERT_NE(b, nullptr);

  EXPECT_TRUE(result.isMaybeNull(after, x));
  EXPECT_TRUE(result.isMaybeNullMemory(after, b));
}

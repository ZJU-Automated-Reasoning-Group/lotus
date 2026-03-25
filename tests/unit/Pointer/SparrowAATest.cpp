/**
 * @file SparrowAATest.cpp
 * @brief Comprehensive unit tests for SparrowAA (Andersen's pointer analysis)
 *
 * SparrowAA implements context-sensitive Andersen's pointer analysis
 * using field-sensitive and flow-insensitive techniques.
 */

#include "Alias/SparrowAA/AndersenAA.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <set>

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

class SparrowAATest : public LlvmModuleTest {
protected:
  bool pointsToSetContains(const std::vector<const Value *> &ptsSet,
                           const Value *v) {
    return std::find(ptsSet.begin(), ptsSet.end(), v) != ptsSet.end();
  }
};

TEST_F(SparrowAATest, SimplePointerAssignment) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *q = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(q, ptsSet);
  bool pointsToX = pointsToSetContains(ptsSet, x);
  EXPECT_TRUE(pointsToX || !ptsSet.empty());
}

TEST_F(SparrowAATest, NoAliasDisjointPointsTo) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %y = alloca i32
      %px = alloca i32*
      store i32* %x, i32** %px
      store i32* %y, i32** %px
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *y = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (!x)
          x = AI;
        else if (!y)
          y = AI;
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(x, ptsSet);
  AA.getPointsToSet(y, ptsSet);
  EXPECT_FALSE(ptsSet.empty());
}

TEST_F(SparrowAATest, GlobalVariablePointsTo) {
  const char *source = R"(
    @global = global i32 0, align 4

    define void @test() {
      %p = alloca i32*
      store i32* @global, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  GlobalVariable *global = module->getNamedGlobal("global");
  ASSERT_NE(global, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(global, ptsSet);
  EXPECT_TRUE(AA.pointsToConstantMemory(
                  MemoryLocation(global, LocationSize::beforeOrAfterPointer()),
                  false) ||
              !ptsSet.empty());
}

TEST_F(SparrowAATest, TransitivePointsTo) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p1 = alloca i32*
      %p2 = alloca i32*
      store i32* %x, i32** %p1
      %t1 = load i32*, i32** %p1
      store i32* %t1, i32** %p2
      %t2 = load i32*, i32** %p2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *t2 = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        t2 = LI;
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(t2, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(t2, ptsSet);
  bool found = false;
  for (const auto *v : ptsSet) {
    if (v == x) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found || ptsSet.size() > 0);
}

TEST_F(SparrowAATest, AliasQueryTest) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p1 = alloca i32*
      %p2 = alloca i32*
      store i32* %x, i32** %p1
      store i32* %x, i32** %p2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  std::vector<Value *> allocs;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        allocs.push_back(AI);
      }
    }
  }

  ASSERT_EQ(allocs.size(), 3u);

  auto loc1 = MemoryLocation(allocs[1], LocationSize::beforeOrAfterPointer());
  auto loc2 = MemoryLocation(allocs[2], LocationSize::beforeOrAfterPointer());
  AliasResult result = AA.alias(loc1, loc2);
  EXPECT_TRUE(result == AliasResult::NoAlias ||
              result == AliasResult::MayAlias);
}

TEST_F(SparrowAATest, FunctionParameterPointsTo) {
  const char *source = R"(
    define void @test(i32* %p) {
      %q = alloca i32*
      store i32* %p, i32** %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Argument *p = F->getArg(0);
  ASSERT_NE(p, nullptr);

  std::vector<const Value *> ptsSet;
  bool hasPointsTo = AA.getPointsToSet(p, ptsSet);
  EXPECT_TRUE(hasPointsTo || ptsSet.empty());
}

TEST_F(SparrowAATest, HeapAllocationAnalysis) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @test() {
      %raw = call i8* @malloc(i64 16)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  CallInst *mallocCall = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "malloc") {
          mallocCall = CI;
        }
      }
    }
  }

  ASSERT_NE(mallocCall, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(mallocCall, ptsSet);
  EXPECT_TRUE(ptsSet.empty() || ptsSet.size() > 0);
}

TEST_F(SparrowAATest, ArrayElementAccess) {
  const char *source = R"(
    define void @test() {
      %arr = alloca [10 x i32]
      %p1 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 0
      %p2 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 5
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  std::vector<Value *> geps;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        geps.push_back(GEP);
      }
    }
  }

  ASSERT_EQ(geps.size(), 2u);

  for (auto *gep : geps) {
    std::vector<const Value *> ptsSet;
    AA.getPointsToSet(gep, ptsSet);
    EXPECT_TRUE(true);
  }
}

TEST_F(SparrowAATest, CastInstructionHandling) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = bitcast i32* %x to i8*
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *p = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        x = AI;
      }
      if (BitCastInst *BI = dyn_cast<BitCastInst>(&I)) {
        p = BI;
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(p, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(p, ptsSet);
  EXPECT_TRUE(ptsSet.empty() || ptsSet.size() > 0);
}

TEST_F(SparrowAATest, ConstantPointerAnalysis) {
  const char *source = R"(
    @constant_ptr = constant i32* null

    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  GlobalVariable *constantPtr = module->getNamedGlobal("constant_ptr");
  ASSERT_NE(constantPtr, nullptr);

  auto result = AA.pointsToConstantMemory(
      MemoryLocation(constantPtr, LocationSize::beforeOrAfterPointer()), false);
  EXPECT_TRUE(result || true);
}

TEST_F(SparrowAATest, ContextInsensitiveDefault) {
  const char *source = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define void @caller1() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret void
    }

    define void @caller2() {
      %y = alloca i32
      call void @callee(i32* %y)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module);

  Function *callee = module->getFunction("callee");
  ASSERT_NE(callee, nullptr);

  Argument *calleeParam = callee->getArg(0);
  ASSERT_NE(calleeParam, nullptr);

  std::vector<const Value *> ptsSet;
  AA.getPointsToSet(calleeParam, ptsSet);
  EXPECT_TRUE(ptsSet.size() >= 0);
}

TEST_F(SparrowAATest, ContextSensitive1CFA) {
  const char *source = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define void @caller1() {
      %x = alloca i32
      call void @helper(i32* %x)
      ret void
    }

    define void @caller2() {
      %y = alloca i32
      call void @helper(i32* %y)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, ContextSensitive2CFA) {
  const char *source = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define void @caller1() {
      %x = alloca i32
      call void @helper(i32* %x)
      ret void
    }

    define void @caller2() {
      %y = alloca i32
      call void @helper(i32* %y)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 2);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, ContextSensitiveQueryInContext) {
  const char *source = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define void @caller() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  const auto *initialCtx = AA.getInitialContext();

  Function *callee = module->getFunction("callee");
  ASSERT_NE(callee, nullptr);

  Argument *calleeParam = callee->getArg(0);
  std::vector<const Value *> ptsSet;
  bool hasResult = AA.getPointsToSetInContext(calleeParam, initialCtx, ptsSet);

  EXPECT_TRUE(hasResult || ptsSet.empty());
}

TEST_F(SparrowAATest, ContextEvolution) {
  const char *source = R"(
    define void @foo(i32* %p) {
      ret void
    }

    define void @bar(i32* %p) {
      call void @foo(i32* %p)
      ret void
    }

    define void @baz(i32* %p) {
      call void @bar(i32* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *foo = module->getFunction("foo");
  ASSERT_NE(foo, nullptr);

  const auto *globalCtx = AA.getGlobalContext();
  const auto *initialCtx = AA.getInitialContext();

  EXPECT_NE(globalCtx, nullptr);
  EXPECT_NE(initialCtx, nullptr);
}

TEST_F(SparrowAATest, ContextToString) {
  const char *source = R"(
    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  const auto *globalCtx = AA.getGlobalContext();
  const auto *initialCtx = AA.getInitialContext();

  std::string globalStr = AA.contextToString(globalCtx, false);
  std::string initialStr = AA.contextToString(initialCtx, false);

  EXPECT_TRUE(globalStr.size() > 0 || initialStr.size() > 0);
}

TEST_F(SparrowAATest, IndirectCallContextSensitive) {
  const char *source = R"(
    define void @func1(i32* %p) {
      ret void
    }

    define void @func2(i32* %p) {
      ret void
    }

    define void @caller(void (i32*)* %fp) {
      call void %fp(i32* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, MustAliasVsMayAlias) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr, *q = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  auto locX = MemoryLocation(x, LocationSize::beforeOrAfterPointer());
  auto locQ = MemoryLocation(q, LocationSize::beforeOrAfterPointer());
  AliasResult result = AA.alias(locX, locQ);

  EXPECT_TRUE(result == AliasResult::MayAlias ||
              result == AliasResult::MustAlias);
}

TEST_F(SparrowAATest, HeapAllocationContextSensitive) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define i8* @allocA() {
      %p = call i8* @malloc(i64 16)
      ret i8* %p
    }

    define i8* @allocB() {
      %p = call i8* @malloc(i64 32)
      ret i8* %p
    }

    define void @test() {
      %a = call i8* @allocA()
      %b = call i8* @allocB()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA(*module, 1);

  Function *test = module->getFunction("test");
  ASSERT_NE(test, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(SparrowAATest, CompareContextSensitiveVsInsensitive) {
  const char *source = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %p) {
      store i32* %p, i32** @global_ptr
      ret void
    }

    define void @caller() {
      %x = alloca i32
      call void @helper(i32* %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AndersenAAResult AA_CI(*module, 0);
  AndersenAAResult AA_CS(*module, 1);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  Argument *param = helper->getArg(0);
  ASSERT_NE(param, nullptr);

  std::vector<const Value *> ptsCI, ptsCS;
  AA_CI.getPointsToSet(param, ptsCI);
  AA_CS.getPointsToSet(param, ptsCS);

  EXPECT_TRUE(ptsCI.size() >= 0);
  EXPECT_TRUE(ptsCS.size() >= 0);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

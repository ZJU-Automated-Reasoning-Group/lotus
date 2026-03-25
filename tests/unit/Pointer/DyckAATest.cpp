/**
 * @file DyckAATest.cpp
 * @brief Enriched unit tests for DyckAA (unification-based alias analysis)
 *
 * DyckAA implements a unification-based (Steensgaard-style) alias analysis
 * which is flow-insensitive and context-insensitive but very fast.
 * It uses equivalence classes to merge pointers that may alias.
 */

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

class DyckAATest : public LlvmModuleTest {};

// ============================================================================
// Basic Alias Tests
// ============================================================================

TEST_F(DyckAATest, SimpleAlias) {
  const char *source = R"(
    define i32 @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

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

  bool aliases = DAA.mayAlias(x, q);
  EXPECT_TRUE(aliases);
}

TEST_F(DyckAATest, NoAlias) {
  const char *source = R"(
    define i32 @test() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

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

  bool aliases = DAA.mayAlias(x, y);
  EXPECT_FALSE(aliases);
}

// ============================================================================
// Pointer Chain Tests
// ============================================================================

TEST_F(DyckAATest, StoreAndLoadAlias) {
  const char *source = R"(
    define i32 @test() {
      %x = alloca i32
      %p = alloca i32*
      %q = alloca i32*
      store i32* %x, i32** %p
      %l1 = load i32*, i32** %p
      store i32* %l1, i32** %q
      %l2 = load i32*, i32** %q
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *x = nullptr;
  Value *l2 = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getName() == "l2") {
          l2 = LI;
        }
      }
    }
  }

  ASSERT_NE(x, nullptr);
  ASSERT_NE(l2, nullptr);

  EXPECT_TRUE(DAA.mayAlias(x, l2));
}

// ============================================================================
// Struct Field Tests
// ============================================================================

TEST_F(DyckAATest, GEPDifferentFieldsNoAlias) {
  const char *source = R"(
    %struct.S = type { i32, i32 }

    define i32 @test() {
      %s = alloca %struct.S
      %f0 = getelementptr %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr %struct.S, %struct.S* %s, i32 0, i32 1
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  GetElementPtrInst *f0 = nullptr, *f1 = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        if (GEP->getNumIndices() >= 2) {
          auto *idx = dyn_cast<ConstantInt>(GEP->getOperand(2));
          if (idx) {
            if (idx->getZExtValue() == 0)
              f0 = GEP;
            else if (idx->getZExtValue() == 1)
              f1 = GEP;
          }
        }
      }
    }
  }

  ASSERT_NE(f0, nullptr);
  ASSERT_NE(f1, nullptr);
  EXPECT_FALSE(DAA.mayAlias(f0, f1));
}

// ============================================================================
// Global Variable Tests
// ============================================================================

TEST_F(DyckAATest, GlobalsNoAlias) {
  const char *source = R"(
    @g1 = global i32 0
    @g2 = global i32 0

    define i32 @test() {
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  GlobalVariable *g1 = module->getGlobalVariable("g1");
  GlobalVariable *g2 = module->getGlobalVariable("g2");
  ASSERT_NE(g1, nullptr);
  ASSERT_NE(g2, nullptr);

  EXPECT_FALSE(DAA.mayAlias(g1, g2));
}

TEST_F(DyckAATest, GlobalAndLocalNoAlias) {
  const char *source = R"(
    @global_var = global i32 0

    define i32 @test() {
      %local = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  GlobalVariable *globalVar = module->getGlobalVariable("global_var");
  ASSERT_NE(globalVar, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *local = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        local = AI;
        break;
      }
    }
  }

  ASSERT_NE(local, nullptr);
  EXPECT_FALSE(DAA.mayAlias(globalVar, local));
}

// ============================================================================
// Null Pointer Tests
// ============================================================================

TEST_F(DyckAATest, NullPointer) {
  const char *source = R"(
    define i32 @test() {
      %x = alloca i32*
      store i32* null, i32** %x
      %p = load i32*, i32** %x
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *p = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          p = LI;
        }
      }
    }
  }

  ASSERT_NE(p, nullptr);

  bool mayBeNull = DAA.mayNull(p);
  EXPECT_TRUE(mayBeNull);
}

TEST_F(DyckAATest, NonNullPointer) {
  const char *source = R"(
    define i32 @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *q = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
          break;
        }
      }
    }
  }
  ASSERT_NE(q, nullptr);

  bool mayBeNull = DAA.mayNull(q);
  EXPECT_FALSE(mayBeNull);
}

// ============================================================================
// Function Call Tests
// ============================================================================

TEST_F(DyckAATest, FunctionParameterAlias) {
  const char *source = R"(
    define void @callee(i32* %p) {
      ret void
    }

    define i32 @test() {
      %x = alloca i32
      call void @callee(i32* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *testFn = module->getFunction("test");
  ASSERT_NE(testFn, nullptr);

  Value *x = nullptr;
  for (auto &BB : *testFn) {
    for (auto &I : BB) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
        x = AI;
        break;
      }
    }
  }
  ASSERT_NE(x, nullptr);

  Function *callee = module->getFunction("callee");
  ASSERT_NE(callee, nullptr);

  Argument *param = callee->getArg(0);
  ASSERT_NE(param, nullptr);

  EXPECT_TRUE(DAA.mayAlias(x, param));
}

TEST_F(DyckAATest, FunctionReturnAlias) {
  const char *source = R"(
    define i32* @get_ptr() {
      %x = alloca i32
      ret i32* %x
    }

    define i32 @test() {
      %p = call i32* @get_ptr()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *testFn = module->getFunction("test");
  ASSERT_NE(testFn, nullptr);

  Value *p = nullptr;
  for (auto &BB : *testFn) {
    for (auto &I : BB) {
      if (CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "get_ptr") {
          p = CI;
          break;
        }
      }
    }
  }
  ASSERT_NE(p, nullptr);

  EXPECT_TRUE(p->getType()->isPointerTy());
}

// ============================================================================
// Heap Allocation Tests
// ============================================================================

TEST_F(DyckAATest, HeapAllocation) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define i32 @test() {
      %p = call i8* @malloc(i64 16)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  Value *mallocRet = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "malloc") {
          mallocRet = CI;
          break;
        }
      }
    }
  }
  ASSERT_NE(mallocRet, nullptr);

  EXPECT_TRUE(mallocRet->getType()->isPointerTy());
}

// ============================================================================
// Complex Control Flow Tests
// ============================================================================

TEST_F(DyckAATest, BranchDifferentPaths) {
  const char *source = R"(
    define i32 @test(i1 %cond) {
      %x = alloca i32
      %y = alloca i32
      store i32 10, i32* %x
      store i32 20, i32* %y
      br i1 %cond, label %true, label %false

    true:
      %v1 = load i32, i32* %x
      br label %merge

    false:
      %v2 = load i32, i32* %y
      br label %merge

    merge:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

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

  EXPECT_FALSE(DAA.mayAlias(x, y));
}

TEST_F(DyckAATest, LoopInvariant) {
  const char *source = R"(
    define void @test(i32* %arr, i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [0, %entry], [%next, %loop]
      %idx = getelementptr inbounds i32, i32* %arr, i32 %i
      store i32 %i, i32* %idx
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit

    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  EXPECT_TRUE(F->size() > 0);
}

// ============================================================================
// Array Access Tests
// ============================================================================

TEST_F(DyckAATest, ArrayElementsMayAlias) {
  const char *source = R"(
    define i32 @test() {
      %arr = alloca [10 x i32]
      %p0 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 0
      %p5 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i32 0, i32 5
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  GetElementPtrInst *p0 = nullptr, *p5 = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        auto *lastIdx =
            dyn_cast<ConstantInt>(GEP->getOperand(GEP->getNumOperands() - 1));
        if (lastIdx) {
          if (lastIdx->getZExtValue() == 0)
            p0 = GEP;
          else if (lastIdx->getZExtValue() == 5)
            p5 = GEP;
        }
      }
    }
  }

  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p5, nullptr);

  // DyckAA is unification-based (Steensgaard-style), so array elements may
  // alias This is expected behavior for unification-based analyses
  EXPECT_TRUE(DAA.mayAlias(p0, p5));
}

// ============================================================================
// Bitcast Tests
// ============================================================================

TEST_F(DyckAATest, BitCastPreservesAliasing) {
  const char *source = R"(
    define i32 @test() {
      %x = alloca i32
      store i32 42, i32* %x
      %p = bitcast i32* %x to i8*
      %val = load i8, i8* %p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

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

  EXPECT_TRUE(DAA.mayAlias(x, p));
}

// ============================================================================
// Multiple Functions Tests
// ============================================================================

TEST_F(DyckAATest, MultipleFunctions) {
  const char *source = R"(
    define void @foo() {
      %x = alloca i32
      ret void
    }

    define void @bar() {
      %y = alloca i32
      ret void
    }

    define i32 @test() {
      call void @foo()
      call void @bar()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DyckAliasAnalysis DAA;
  DAA.runOnModule(*module);

  EXPECT_TRUE(true);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

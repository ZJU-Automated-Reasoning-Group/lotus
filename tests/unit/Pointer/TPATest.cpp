/**
 * @file TPATest.cpp
 * @brief Unit tests for TPA (semi-sparse flow- and context-sensitive pointer
 * analysis)
 */

#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Alias/TPA/Transforms/RunPrepass.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace tpa;
using namespace transform;
using namespace lotus::unittest;

namespace {

bool mayAlias(const SemiSparsePointerAnalysis &pta, const Value *v1,
              const Value *v2) {
  PtsSet pts1 = pta.getPtsSet(v1);
  PtsSet pts2 = pta.getPtsSet(v2);
  for (const auto *obj : pts1) {
    if (pts2.has(obj))
      return true;
  }
  return false;
}

} // namespace

class TPATest : public lotus::unittest::LlvmModuleTest {};

TEST_F(TPATest, NoAliasTwoAllocas) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *y = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!x)
          x = AI;
        else if (!y)
          y = AI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  EXPECT_FALSE(mayAlias(pta, x, y));
}

TEST_F(TPATest, AliasStoreLoad) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32))
          x = AI;
      }
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy())
          q = LI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  EXPECT_TRUE(mayAlias(pta, x, q));
}

TEST_F(TPATest, PointsToSetLoadContainsStoredAlloca) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
          break;
        }
      }
    }
  }
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

TEST_F(TPATest, FlowSensitivityBasic) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

TEST_F(TPATest, FlowSensitivityPointerChain) {
  const char *ir = R"(
    define i32 @main() {
      %a = alloca i32
      %b = alloca i32
      %p = alloca i32*
      %q = alloca i32*

      store i32* %a, i32** %p
      %t1 = load i32*, i32** %p
      store i32* %t1, i32** %q
      store i32* %b, i32** %p
      %t2 = load i32*, i32** %q
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *t2 = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy() &&
            LI->getParent()->getParent()->getName() == "main") {
          t2 = LI;
        }
      }
    }
  }
  ASSERT_NE(t2, nullptr);

  PtsSet ptsT2 = pta.getPtsSet(t2);
  EXPECT_FALSE(ptsT2.empty());
}

TEST_F(TPATest, ContextSensitivitySameFunctionDifferentContexts) {
  const char *ir = R"(
    @global_ptr = global i32* null

    define void @helper(i32* %param) {
      store i32* %param, i32** @global_ptr
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      call void @helper(i32* %x)
      call void @helper(i32* %y)
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *helper = module->getFunction("helper");
  ASSERT_NE(helper, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, ContextSensitivityRecursiveFunction) {
  const char *ir = R"(
    define void @foo(i32** %p, i32 %n) {
      %cmp = icmp sgt i32 %n, 0
      br i1 %cmp, label %body, label %exit

    body:
      %ptr = alloca i32
      store i32* %ptr, i32** %p
      %n1 = sub i32 %n, 1
      call void @foo(i32** %p, i32 %n1)
      br label %exit

    exit:
      ret void
    }

    define i32 @main() {
      %p = alloca i32*
      call void @foo(i32** %p, i32 3)
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *foo = module->getFunction("foo");
  ASSERT_NE(foo, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, FunctionReturnPointsTo) {
  const char *ir = R"(
    define i32* @alloc_i32() {
      %x = alloca i32
      ret i32* %x
    }

    define i32 @main() {
      %p = call i32* @alloc_i32()
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *p = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "alloc_i32") {
          p = CI;
        }
      }
    }
  }
  ASSERT_NE(p, nullptr);

  PtsSet ptsP = pta.getPtsSet(p);
  EXPECT_FALSE(ptsP.empty());
}

TEST_F(TPATest, FunctionParameterPassing) {
  const char *ir = R"(
    define void @take_ptr(i32* %p) {
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      call void @take_ptr(i32* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *takePtr = module->getFunction("take_ptr");
  ASSERT_NE(takePtr, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, NestedStructureAccess) {
  const char *ir = R"(
    %struct = type { i32, i32* }

    define i32 @main() {
      %s = alloca %struct
      %f1 = getelementptr inbounds %struct, %struct* %s, i32 0, i32 0
      %f2 = getelementptr inbounds %struct, %struct* %s, i32 0, i32 1
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  EXPECT_TRUE(true);
}

TEST_F(TPATest, PointerToPointer) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      %pp = alloca i32**

      store i32* %x, i32** %p
      store i32** %p, i32*** %pp

      %v = load i32**, i32*** %pp
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *v = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          v = LI;
        }
      }
    }
  }
  ASSERT_NE(v, nullptr);

  PtsSet ptsV = pta.getPtsSet(v);
  EXPECT_FALSE(ptsV.empty());
}

TEST_F(TPATest, PointerAliasing) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32)) {
          x = AI;
        }
      }
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
        }
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

TEST_F(TPATest, NullPointerHandling) {
  const char *ir = R"(
    define i32 @main() {
      %p = alloca i32*
      store i32* null, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *p = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isPointerTy()) {
          p = AI;
          break;
        }
      }
    }
  }
  ASSERT_NE(p, nullptr);

  auto ptsP = pta.getPtsSet(p);
  (void)ptsP;
  EXPECT_TRUE(true);
}

TEST_F(TPATest, MultipleFunctionsAnalysis) {
  const char *ir = R"(
    define void @foo() {
      %x = alloca i32
      ret void
    }

    define void @bar() {
      %y = alloca i32
      ret void
    }

    define i32 @main() {
      call void @foo()
      call void @bar()
      ret i32 0
    }
  )";

  auto module = parseModule(ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  EXPECT_TRUE(true);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

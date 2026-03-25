#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"
#include "TestUtils/LLVMHelpers.h"
#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class RAIIMultiLifetimeTest : public LlvmModuleTest {
protected:
  using LlvmModuleTest::parseModule;
};

TEST_F(RAIIMultiLifetimeTest, LoopRAII) {
  const char *source = R"(
    %struct.mutex = type { i32 }
    %struct.lock_guard = type { %struct.mutex* }

    declare void @mutex_lock(%struct.mutex*)
    declare void @mutex_unlock(%struct.mutex*)
    declare void @guard_ctor(%struct.lock_guard*, %struct.mutex*)
    declare void @guard_dtor(%struct.lock_guard*)

    @m = global %struct.mutex zeroinitializer, align 4

    define void @test_loop(i32 %n) {
    entry:
      br label %loop
    loop:
      %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
      %guard = alloca %struct.lock_guard
      call void @guard_ctor(%struct.lock_guard* %guard, %struct.mutex* @m)
      ; critical section
      %val = load i32, i32* @m
      call void @guard_dtor(%struct.lock_guard* %guard)
      %i.next = add i32 %i, 1
      %cmp = icmp slt i32 %i.next, %n
      br i1 %cmp, label %loop, label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  LockSetAnalysis lsa(*module);
  lsa.analyze();

  const Function *func = module->getFunction("test_loop");
  Instruction *loadM = nullptr;
  for (auto &BB : *func) {
    for (auto &I : BB) {
      if (isa<LoadInst>(I)) {
        loadM = &I;
        break;
      }
    }
  }
  ASSERT_NE(loadM, nullptr);

  EXPECT_TRUE(lsa.mustHoldLock(loadM, module->getNamedGlobal("m")));
}

#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"
#include "TestUtils/LLVMHelpers.h"
#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class AtomicNonConstantTest : public LlvmModuleTest {
protected:
  using LlvmModuleTest::parseModule;
};

TEST_F(AtomicNonConstantTest, DynamicAtomicSync) {
  const char *source = R"(
    @a = global i32 0, align 4
    @b = global i32 0, align 4
    @atomic_var = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @thread_a(i8* %arg) {
    entry:
      store i32 1, i32* @a, align 4
      %val = load i32, i32* @b, align 4
      store atomic i32 %val, i32* @atomic_var release, align 4
      ret i8* null
    }

    define i8* @thread_b(i8* %arg) {
    entry:
      %sync = load atomic i32, i32* @atomic_var acquire, align 4
      %cmp = icmp ne i32 %sync, 0
      br i1 %cmp, label %safe, label %exit
    safe:
      store i32 2, i32* @a, align 4
      ret i8* null
    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread_a, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread_b, i8* null)
      call i32 @pthread_join(i8* %tid1, i8* null)
      call i32 @pthread_join(i8* %tid2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *thread_a = module->getFunction("thread_a");
  const Function *thread_b = module->getFunction("thread_b");
  
  Instruction *store_a1 = nullptr;
  for (auto &I : thread_a->getEntryBlock()) {
    if (isa<StoreInst>(I) && !CppAtomics::isAtomic(&I)) {
      store_a1 = &I;
      break;
    }
  }

  Instruction *store_a2 = nullptr;
  for (auto &BB : *thread_b) {
    if (BB.getName() == "safe") {
      for (auto &I : BB) {
        if (isa<StoreInst>(I) && !CppAtomics::isAtomic(&I)) {
          store_a2 = &I;
          break;
        }
      }
    }
  }

  ASSERT_NE(store_a1, nullptr);
  ASSERT_NE(store_a2, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_a1, store_a2));
}

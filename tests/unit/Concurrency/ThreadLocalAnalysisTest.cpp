#include "Analysis/Concurrency/Utils/ThreadLocalAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace ThreadLocal;
using namespace lotus::unittest;

class ThreadLocalAnalysisTest : public LlvmModuleTest {
protected:
};

TEST_F(ThreadLocalAnalysisTest, StoreThroughStackGepStaysThreadLocal) {
  const char *source = R"(
    %struct.S = type { i32, i32 }

    define i32 @main() {
    entry:
      %obj = alloca %struct.S, align 4
      %field = getelementptr inbounds %struct.S, %struct.S* %obj, i32 0, i32 1
      store i32 7, i32* %field, align 4
      %load = load i32, i32* %field, align 4
      ret i32 %load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *load = findInstructionByName(*main_func, "load");
  ASSERT_NE(load, nullptr);

  EXPECT_TRUE(tla.accessesThreadLocalStorage(load));
}

TEST_F(ThreadLocalAnalysisTest, InvokeGetspecificIsRecognizedAsThreadLocal) {
  const char *source = R"(
    declare i8* @pthread_getspecific(i32)
    declare i32 @__gxx_personality_v0(...)

    define i32 @main() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %tls = invoke i8* @pthread_getspecific(i32 0)
              to label %cont unwind label %lpad

    cont:
      %use = ptrtoint i8* %tls to i64
      ret i32 0

    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *tls = findInstructionByName(*main_func, "tls");
  ASSERT_NE(tls, nullptr);

  EXPECT_TRUE(tla.isThreadLocal(tls));
}

TEST_F(ThreadLocalAnalysisTest, LoadFromTlsSlotDoesNotMakeSharedPointeeThreadLocal) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i8* @pthread_getspecific(i32)

    define i32 @main() {
    entry:
      %slot = call i8* @pthread_getspecific(i32 0)
      %typed = bitcast i8* %slot to i32**
      store i32* @shared, i32** %typed, align 8
      %loaded_ptr = load i32*, i32** %typed, align 8
      %loaded_val = load i32, i32* %loaded_ptr, align 4
      ret i32 %loaded_val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *loaded_ptr = findInstructionByName(*main_func, "loaded_ptr");
  const Instruction *loaded_val = findInstructionByName(*main_func, "loaded_val");
  ASSERT_NE(loaded_ptr, nullptr);
  ASSERT_NE(loaded_val, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(loaded_ptr));
  EXPECT_FALSE(tla.accessesThreadLocalStorage(loaded_val));
}

TEST_F(ThreadLocalAnalysisTest, PthreadHandleStorageRemainsThreadLocal) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *tid = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(tid, nullptr);

  EXPECT_TRUE(tla.isThreadLocal(tid));
}

TEST_F(ThreadLocalAnalysisTest, HelperMediatedThreadPayloadIsNotThreadLocal) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @spawn_helper(i8* %tid, i32* %payload, i8* (i8*)* %fn) {
    entry:
      %payload_raw = bitcast i32* %payload to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn,
                               i8* %payload_raw)
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      %tid = alloca i8, align 1
      call void @spawn_helper(i8* %tid, i32* %slot, i8* (i8*)* @worker)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(slot));
}

TEST_F(ThreadLocalAnalysisTest,
       LocalPointerCarrierDoesNotHideThreadPayloadEscape) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* %arg
    }

    define i32 @main() {
    entry:
      %shared = alloca i32, align 4
      %carrier = alloca i32*, align 8
      %tid = alloca i8, align 1
      store i32* %shared, i32** %carrier, align 8
      %loaded = load i32*, i32** %carrier, align 8
      %payload = bitcast i32* %loaded to i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* %payload)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadLocalAnalysis tla(*module);
  tla.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *shared = dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(shared, nullptr);

  EXPECT_FALSE(tla.isThreadLocal(shared));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif

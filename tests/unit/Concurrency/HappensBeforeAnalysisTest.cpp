#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class HappensBeforeAnalysisTest : public LlvmModuleTest {
protected:
  using LlvmModuleTest::parseModule;
};

TEST_F(HappensBeforeAnalysisTest, TwoThreadsNoSync_NeitherHappensBefore) {
  const char *source = R"(
    @x = global i32 0, align 4
    @y = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @thread_a(i8* %arg) {
    entry:
      store i32 1, i32* @x, align 4
      ret i8* null
    }

    define i8* @thread_b(i8* %arg) {
    entry:
      store i32 2, i32* @y, align 4
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
  ASSERT_NE(thread_a, nullptr);
  ASSERT_NE(thread_b, nullptr);

  const Instruction *store_a = &thread_a->getEntryBlock().front();
  const Instruction *store_b = &thread_b->getEntryBlock().front();
  ASSERT_NE(store_a, nullptr);
  ASSERT_NE(store_b, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_a, store_b));
  EXPECT_FALSE(hb.happensBefore(store_b, store_a));
  EXPECT_FALSE(hb.happensBefore(store_a, store_a));
}

TEST_F(HappensBeforeAnalysisTest, MultiExitWorkerStillHappensBeforePostJoin) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %cond = icmp eq i8* %arg, null
      br i1 %cond, label %left, label %right

    left:
      %left_work = add i32 1, 2
      ret i8* null

    right:
      %right_work = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* undef)
      call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *worker_func = module->getFunction("worker");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(worker_func, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *left_work =
      findInstructionByName(*worker_func, "left_work");
  const Instruction *right_work =
      findInstructionByName(*worker_func, "right_work");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(left_work, nullptr);
  ASSERT_NE(right_work, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(hb.happensBefore(left_work, post));
  EXPECT_TRUE(hb.happensBefore(right_work, post));
}

TEST_F(HappensBeforeAnalysisTest, MutexHandoffAcrossForkCreatesHB) {
  const char *source = R"(
    @lock = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %seen = load i32, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_mutex_lock(i8* @lock)
      store i32 42, i32* @shared, align 4
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_mutex_unlock(i8* @lock)
      call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *store_shared = nullptr;
  for (const Instruction &inst : instructions(*main_func)) {
    if (isa<StoreInst>(&inst)) {
      store_shared = &inst;
      break;
    }
  }
  const Instruction *load_shared = findInstructionByName(*worker_func, "seen");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       CompetingPeerMutexCriticalSectionsDoNotInventHB) {
  const char *source = R"(
    @lock = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      store i32 1, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %seen = load i32, i32* @shared, align 4
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store_shared = &inst;
      break;
    }
  }
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("worker2"), "seen");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
  EXPECT_FALSE(hb.happensBefore(load_shared, store_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       UniqueConditionSignalCreatesHBForSingleWaiter) {
  const char *source = R"(
    @cond = global i8 0
    @mutex = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    declare i32 @pthread_cond_signal(i8*)

    define i8* @waiter(i8* %arg) {
    entry:
      call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %seen = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @signaler(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      call i32 @pthread_cond_signal(i8* @cond)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @waiter, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @signaler, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("signaler")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("waiter"), "seen");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("condvar_sync_edges");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       MultipleWaitersOnSameConditionRemainDeferred) {
  const char *source = R"(
    @cond = global i8 0
    @mutex = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    declare i32 @pthread_cond_signal(i8*)

    define i8* @waiter1(i8* %arg) {
    entry:
      call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %seen1 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @waiter2(i8* %arg) {
    entry:
      call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %seen2 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @signaler(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      call i32 @pthread_cond_signal(i8* @cond)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @waiter1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @waiter2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @signaler, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("signaler")->getEntryBlock().front();
  const Instruction *load1 =
      findInstructionByName(*module->getFunction("waiter1"), "seen1");
  const Instruction *load2 =
      findInstructionByName(*module->getFunction("waiter2"), "seen2");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load1, nullptr);
  ASSERT_NE(load2, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load1));
  EXPECT_FALSE(hb.happensBefore(store_shared, load2));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("condvar_relations_deferred");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest, CallOnceDoesNotCreateBidirectionalHB) {
  const char *source = R"(
    @flag = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag)
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag)
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *worker1 = module->getFunction("worker1");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(worker1, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *w1 = findInstructionByName(*worker1, "w1");
  const Instruction *w2 = findInstructionByName(*worker2, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  EXPECT_FALSE(hb.happensBefore(w1, w2) && hb.happensBefore(w2, w1));
}

TEST_F(HappensBeforeAnalysisTest, CallOnceCallbackSynchronizesWithFollowers) {
  const char *source = R"(
    @flag = global i8 0
    @shared = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*, void ()*)

    define void @init_once() {
    entry:
      store i32 7, i32* @shared
      ret void
    }

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      %v = load i32, i32* @shared
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *init_once = module->getFunction("init_once");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(init_once, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *store_shared = &init_once->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*worker2, "v");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       CallOnceWithDifferentCallbacksDoesNotInventCrossCallbackHB) {
  const char *source = R"(
    @flag = global i8 0
    @a = global i32 0
    @b = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*, void ()*)

    define void @init_a() {
    entry:
      store i32 7, i32* @a
      ret void
    }

    define void @init_b() {
    entry:
      store i32 9, i32* @b
      ret void
    }

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_a)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_b)
      %v = load i32, i32* @a
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_a =
      &module->getFunction("init_a")->getEntryBlock().front();
  const Instruction *load_a =
      findInstructionByName(*module->getFunction("worker2"), "v");
  ASSERT_NE(store_a, nullptr);
  ASSERT_NE(load_a, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_a, load_a));
}

TEST_F(HappensBeforeAnalysisTest,
       ReusedCallOnceCallbackDoesNotLeakHBToDirectCalls) {
  const char *source = R"(
    @flag = global i8 0
    @shared = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*, void ()*)

    define void @init_once() {
    entry:
      store i32 7, i32* @shared
      ret void
    }

    define i8* @once_worker(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      ret i8* null
    }

    define i8* @direct_worker(i8* %arg) {
    entry:
      call void @init_once()
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag, void ()* @init_once)
      %v = load i32, i32* @shared
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @once_worker, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @direct_worker, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *init_once = module->getFunction("init_once");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(init_once, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_shared = &init_once->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader, "v");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, PromiseFutureTracksSharedState) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise_obj = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 99, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise_obj)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future = call i8* @_ZNSt7promise10get_futureEv(i8* @promise_obj)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, AsyncFutureGetOrdersAsyncTaskCompletion) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i8* @_ZNSt5async12launch_asyncEv(i32, i8* (i8*)*, i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @worker(i8* %unused) {
    entry:
      store i32 77, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %future = call i8* @_ZNSt5async12launch_asyncEv(
          i32 1, i8* (i8*)* @worker, i8* null)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("main"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       DetachedTaskCompletionOrdersDetachedTaskBeforeFollowerTask) {
  const char *source = R"(
    declare i8* @__kmpc_omp_task_alloc(i8*, i32, i32, i64, i64, void ()*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)

    @shared = global i32 0

    define internal void @detached_body() {
    entry:
      store i32 17, i32* @shared
      ret void
    }

    define internal void @follower_body() {
    entry:
      %v = load i32, i32* @shared
      ret void
    }

    define i32 @main() {
    entry:
      %detached = call i8* @__kmpc_omp_task_alloc(
          i8* null, i32 0, i32 65, i64 32, i64 0, void ()* @detached_body)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %detached)
      call void @__kmpc_omp_task_complete_if0(i8* null, i32 0, i8* %detached)
      call i32 @__kmpc_omp_task(i8* null, i32 0,
          i8* bitcast (void ()* @follower_body to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("detached_body")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("follower_body"), "v");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, PromiseFutureRepeatedQueriesRemainStable) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise_obj = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 99, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise_obj)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future = call i8* @_ZNSt7promise10get_futureEv(i8* @promise_obj)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
  EXPECT_FALSE(hb.happensBefore(load_shared, store_shared));
}

TEST_F(HappensBeforeAnalysisTest, AmbiguousPromiseFutureHandleDoesNotInventHB) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise1 = global i8 0
    @promise2 = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer1(i8* %unused) {
    entry:
      store i32 11, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise1)
      ret i8* null
    }

    define i8* @producer2(i8* %unused) {
    entry:
      call void @_ZNSt7promise9set_valueEv(i8* @promise2)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future1 = call i8* @_ZNSt7promise10get_futureEv(i8* @promise1)
      %future2 = call i8* @_ZNSt7promise10get_futureEv(i8* @promise2)
      %future = select i1 true, i8* %future1, i8* %future2
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @producer2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer1 = module->getFunction("producer1");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer1, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer1->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, LatchWaitSynchronizesAfterCountdown) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @latch = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_latch_count_down(i8*)
    declare void @fake_latch_waitEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 5, i32* @shared, align 4
      call void @fake_latch_count_down(i8* @latch)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      call void @fake_latch_waitEv(i8* @latch)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, LatchArriveAndWaitSynchronizesAfterArrival) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @latch = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_latch_arrive_and_wait(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 13, i32* @shared, align 4
      call void @fake_latch_arrive_and_wait(i8* @latch)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      call void @fake_latch_arrive_and_wait(i8* @latch)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, BarrierWaitSynchronizesAfterArrival) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_barrier_arrive_and_wait(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 8, i32* @shared, align 4
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_shared = &writer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       IncompleteBarrierPhaseDoesNotSynchronizeAfterWait) {
  const char *source = R"(
    @bar = global i8 0
    @shared = global i32 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_init(i8*, i8*, i32)
    declare i32 @pthread_barrier_wait(i8*)

    define i8* @writer(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
      %writer_store = add i32 1, 2
      store i32 %writer_store, i32* @shared, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
      %reader_load = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_barrier_init(i8* @bar, i8* null, i32 3)
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      findInstructionByName(*module->getFunction("writer"), "writer_store");
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "reader_load");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, SplitPhaseBarrierArriveSynchronizesWithWait) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 11, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_shared = &writer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       ReusedSplitPhaseBarrierKeepsBarrierCyclesSeparated) {
  const char *source = R"(
    @first = global i32 0, align 4
    @second = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 1, i32* @first, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      call void @std_barrier_waitEv(i8* @barrier)
      store i32 2, i32* @second, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      call void @std_barrier_waitEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      %first_load = load i32, i32* @first, align 4
      call void @std_barrier_waitEv(i8* @barrier)
      %second_load = load i32, i32* @second, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_first = nullptr;
  const Instruction *store_second = nullptr;
  for (const Instruction &inst : instructions(*writer)) {
    const auto *store = dyn_cast<StoreInst>(&inst);
    if (!store) {
      continue;
    }
    if (store->getPointerOperand() == module->getNamedGlobal("first")) {
      store_first = &inst;
    } else if (store->getPointerOperand() == module->getNamedGlobal("second")) {
      store_second = &inst;
    }
  }
  const Instruction *first_load = findInstructionByName(*reader, "first_load");
  const Instruction *second_load =
      findInstructionByName(*reader, "second_load");
  ASSERT_NE(store_first, nullptr);
  ASSERT_NE(store_second, nullptr);
  ASSERT_NE(first_load, nullptr);
  ASSERT_NE(second_load, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_first, first_load));
  EXPECT_TRUE(hb.happensBefore(store_second, second_load));
  EXPECT_FALSE(hb.happensBefore(store_second, first_load));
}

TEST_F(HappensBeforeAnalysisTest,
       SplitPhaseBarrierWithMultipleParticipantsSynchronizes) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer1(i8* %unused) {
    entry:
      store i32 21, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @writer2(i8* %unused) {
    entry:
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader1(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @reader2(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      %tid4 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @writer2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader1, i8* null)
      call i32 @pthread_create(i8* %tid4, i8* null, i8* (i8*)* @reader2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer1")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader1"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       RepeatedSplitPhaseBarrierWaitsStillOrderLaterContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 31, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
      call void @std_barrier_waitEv(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       BarrierArriveAndWaitSynchronizesPostBarrierContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @barrier = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @fake_barrier_arrive_and_wait(i8*)

    define i8* @writer(i8* %unused) {
    entry:
      store i32 41, i32* @shared, align 4
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %unused) {
    entry:
      call void @fake_barrier_arrive_and_wait(i8* @barrier)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPTaskDependenciesContributeToHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps_out = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps_in = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 1
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep_out = getelementptr inbounds [1 x %kmp_depend_info],
                 [1 x %kmp_depend_info]* @deps_out, i64 0, i64 0
      %dep_in = getelementptr inbounds [1 x %kmp_depend_info],
                [1 x %kmp_depend_info]* @deps_in, i64 0, i64 0
      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep_out, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep_in, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *t1 = findInstructionByName(*main_func, "t1");
  const Instruction *t2 = findInstructionByName(*main_func, "t2");
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);

  EXPECT_TRUE(hb.happensBefore(t1, t2));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPTaskBodyDependenciesContributeToHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps_out = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps_in = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 1
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      %dep_out = getelementptr inbounds [1 x %kmp_depend_info],
                 [1 x %kmp_depend_info]* @deps_out, i64 0, i64 0
      %dep_in = getelementptr inbounds [1 x %kmp_depend_info],
                [1 x %kmp_depend_info]* @deps_in, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task1_raw, i32 1,
          %kmp_depend_info* %dep_out, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task2_raw, i32 1,
          %kmp_depend_info* %dep_in, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPSingleBoundaryOrdersTaskContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)

    define i8* @task_body(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @task_body, i8* (i8*)** %task, align 8
      %task_raw = bitcast i8* (i8*)** %task to i8*
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task_raw)
      %single = call i32 @__kmpc_single(i8* null, i32 0)
      call void @__kmpc_end_single(i8* null, i32 0)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *task_body = module->getFunction("task_body");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(task_body, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *task_store = &task_body->getEntryBlock().front();
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(hb.happensBefore(task_store, after));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPFlushRelationFeedsHBAnalysis) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_flush(i8*)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
             [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task1_raw, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_flush(i8* bitcast (i32* @shared to i8*))
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task2_raw, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       PlainReleaseAcquireWithoutWitnessStaysDeferred) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %val = load i32, i32* @data, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
  EXPECT_FALSE(hb.happensBefore(load_data, store_data));
}

TEST_F(HappensBeforeAnalysisTest,
       DirectReleaseAcquireWithAssumeWitnessCreatesHBWithoutBranch) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.assume(i1)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 23, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      call void @llvm.assume(i1 %ready)
      %val = load i32, i32* @data, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceAcquireFenceWithAssumeWitnessCreatesHBWithoutBranch) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.assume(i1)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 29, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      call void @llvm.assume(i1 %ready)
      %val = load i32, i32* @data, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       AssumeWitnessOnDifferentAtomicLocationDoesNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag1 = global i32 0, align 4
    @flag2 = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.assume(i1)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 37, i32* @data, align 4
      store atomic i32 1, i32* @flag1 release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag2 acquire, align 4
      %ready = icmp ne i32 %seen, 0
      call void @llvm.assume(i1 %ready)
      %val = load i32, i32* @data, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       DirectReleaseAcquireWithBranchWitnessCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  EXPECT_FALSE(hb.happensBefore(load_data, store_data));
}

TEST_F(HappensBeforeAnalysisTest,
       BranchWitnessDoesNotOrderFalsePathContinuation) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %fallback

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    fallback:
      %fallback_val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *true_load =
      findInstructionByName(*module->getFunction("reader"), "val");
  const Instruction *false_load =
      findInstructionByName(*module->getFunction("reader"), "fallback_val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(true_load, nullptr);
  ASSERT_NE(false_load, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, true_load));
  EXPECT_FALSE(hb.happensBefore(store_data, false_load));
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseStoreAcquireFenceWithBranchWitnessCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 13, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest, BranchWitnessMustMatchConcreteReleaseValue) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 2
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       InitialAtomicValueMatchingWitnessDoesNotInventHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 1, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 1
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));

  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_witness_value_incompatible");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseSequenceWithWitnessedConstantCanNowSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 17, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @updater, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_sequence_edges_modeled");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseSequenceWithMultipleTailsCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 27, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater1(i8* %arg) {
    entry:
      %old1 = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @updater2(i8* %arg) {
    entry:
      %old2 = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      %tid4 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @updater1, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @updater2, i8* null)
      call i32 @pthread_create(i8* %tid4, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_sequence_edges_modeled");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       MixedFenceReleaseSequenceWithoutReadsFromProofIsDeferred) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 31, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      fence acquire
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @updater, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  size_t modeled_edges = 0;
  auto mixed_fence_it = deferred.find("atomic_mixed_fence_edges_modeled");
  if (mixed_fence_it != deferred.end()) {
    modeled_edges += mixed_fence_it->second;
  }
  auto release_sequence_it =
      deferred.find("atomic_release_sequence_edges_modeled");
  if (release_sequence_it != deferred.end()) {
    modeled_edges += release_sequence_it->second;
  }
  EXPECT_GT(modeled_edges, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       LaterNonReleaseStoreDoesNotInheritEarlierReleaseHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 19, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @overwriter(i8* %arg) {
    entry:
      store atomic i32 2, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @overwriter, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest, ReleaseFenceStoreAcquireFenceCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  EXPECT_FALSE(hb.happensBefore(load_data, store_data));
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceStoreAcquireFenceDoesNotOrderWitnessLoad) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *seen = findInstructionByName(*reader, "seen");
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(seen, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, seen));
  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceStoreAcquireFenceAcrossBlocksCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      br label %fence_bb

    fence_bb:
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceAcrossBlocksBeforeStoreCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      br label %publish

    publish:
      fence release
      br label %store_bb

    store_bb:
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       MultiStepReleaseSequenceCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @publisher(i8* %arg) {
    entry:
      store i32 77, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @rmw1(i8* %arg) {
    entry:
      %old1 = atomicrmw add i32* @flag, i32 1 monotonic
      ret i8* null
    }

    define i8* @rmw2(i8* %arg) {
    entry:
      %old2 = atomicrmw add i32* @flag, i32 1 monotonic
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 3
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      %tid4 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @publisher, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @rmw1, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @rmw2, i8* null)
      call i32 @pthread_create(i8* %tid4, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("publisher")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       NonAdjacentFenceAtomicPatternInSameBlockCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      %tmp = add i32 1, 2
      store atomic i32 %tmp, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      %tmp = add i32 %seen, 1
      fence acquire
      %ready = icmp ne i32 %tmp, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest, CompetingReleaseStoresDoNotInventAtomicHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer1(i8* %arg) {
    entry:
      store i32 1, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @writer2(i8* %arg) {
    entry:
      store atomic i32 2, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @writer2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer1")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest, CrossLocationAtomicsDoNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag1 = global i32 0, align 4
    @flag2 = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 5, i32* @data, align 4
      store atomic i32 1, i32* @flag1 release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag2 acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest, RelaxedLoadWithoutFenceDoesNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 11, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest,
       SameAggregateDifferentAtomicFieldsDoNotSynchronize) {
  const char *source = R"(
    %Pair = type { i32, i32 }

    @data = global i32 0, align 4
    @pair = global %Pair zeroinitializer, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      %first = getelementptr inbounds %Pair, %Pair* @pair, i32 0, i32 0
      store i32 9, i32* @data, align 4
      store atomic i32 1, i32* %first release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %second = getelementptr inbounds %Pair, %Pair* @pair, i32 0, i32 1
      %seen = load atomic i32, i32* %second acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPTargetDataBoundaryFeedsHBAnalysis) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end(i8*, i32)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task1_raw)
      call i32 @__tgt_target_data_end(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task2_raw)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       NonEqBranchWitnessDoesNotInventDefiniteAtomicHB) {
  const char *source = R"(
    @flag = global i32 0, align 4
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp sgt i32 %seen, 0
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @shared, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       MustAliasAtomicPointersStillSynchronizeAcrossDifferentSSAValues) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      %slot = alloca i32*, align 8
      store i32* @flag, i32** %slot, align 8
      %flag_ptr = load i32*, i32** %slot, align 8
      store i32 1, i32* @data, align 4
      store atomic i32 1, i32* %flag_ptr release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @data, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(load_shared, nullptr);
  const Instruction *writer_store =
      module->getFunction("writer")->getEntryBlock().getFirstNonPHI();
  ASSERT_NE(writer_store, nullptr);

  EXPECT_TRUE(hb.happensBefore(writer_store, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, FailedCmpXchgDoesNotInventDefiniteAtomicHB) {
  const char *source = R"(
    @flag = global i32 0, align 4
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      %res = cmpxchg i32* @flag, i32 0, i32 1 seq_cst monotonic
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag seq_cst, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @shared, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, SuccessfulSeqCstCmpXchgWithWitnessCreatesHB) {
  const char *source = R"(
    @flag = global i32 0, align 4
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      %res = cmpxchg i32* @flag, i32 0, i32 1 seq_cst monotonic
      %ok = extractvalue { i32, i1 } %res, 1
      br i1 %ok, label %success, label %fail

    success:
      br label %done

    fail:
      br label %done

    done:
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag seq_cst, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @shared, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseSequenceWithSingleRmwTailCreatesHBWhenWitnessExcludesInitial) {
  const char *source = R"(
    @flag = global i32 0, align 4
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @tail(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 2
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @shared, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @tail, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_sequence_edges_modeled");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       ReleaseSequenceCompetingStoreRemainsDeferred) {
  const char *source = R"(
    @flag = global i32 0, align 4
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @compete(i8* %arg) {
    entry:
      store atomic i32 9, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @shared, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @compete, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_candidate_unresolved");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(HappensBeforeAnalysisTest,
       MultipleReleaseCandidatesKeepReleaseSequenceDeferred) {
  const char *source = R"(
    @flag = global i32 0, align 4
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer1(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @writer2(i8* %arg) {
    entry:
      store atomic i32 2, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %sync, label %exit

    sync:
      %val = load i32, i32* @shared, align 4
      ret i8* null

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @writer2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer1")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_shared, load_shared));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_candidate_unresolved");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif

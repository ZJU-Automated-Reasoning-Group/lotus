/**
 * @file MHPAnalysisTest.cpp
 * @brief Simplified unit tests for MHP Analysis
 */

#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Analysis/Concurrency/MHP/MHPAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class MHPAnalysisTest : public lotus::unittest::LlvmModuleTest {};

// Test 1: Simple main function
TEST_F(MHPAnalysisTest, SimpleMain) {
  const char *source = R"(
    define i32 @main() {
      %x = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  EXPECT_NO_THROW(mhp.analyze());

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_threads, 0);
}

// Test 2: Thread creation
TEST_F(MHPAnalysisTest, ThreadCreation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    
    define i8* @worker(i8* %arg) {
      ret i8* null
    }
    
    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null, 
                                       i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
}

// Test 3: Lock operations
TEST_F(MHPAnalysisTest, LockOperations) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    
    @lock = global i8 0
    
    define i32 @main() {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %x = add i32 1, 2
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_locks, 1);
  EXPECT_GE(stats.num_unlocks, 1);
}

TEST_F(MHPAnalysisTest, WrapperAndCriticalLocksReachMHPNodes) {
  const char *source = R"(
    @lock = global i8 0
    @crit = global [8 x i32] zeroinitializer

    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define i32 @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  std::vector<const Instruction *> calls;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      calls.push_back(&inst);
    }
  }
  ASSERT_EQ(calls.size(), 4u);
  const Instruction *ctor = calls[0];
  const Instruction *dtor = calls[1];
  const Instruction *critical = calls[2];
  const Instruction *end_critical = calls[3];

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto ctor_nodes = tfg.getNodes(ctor);
  auto dtor_nodes = tfg.getNodes(dtor);
  auto critical_nodes = tfg.getNodes(critical);
  auto end_critical_nodes = tfg.getNodes(end_critical);
  ASSERT_FALSE(ctor_nodes.empty());
  ASSERT_FALSE(dtor_nodes.empty());
  ASSERT_FALSE(critical_nodes.empty());
  ASSERT_FALSE(end_critical_nodes.empty());

  EXPECT_EQ(ctor_nodes.front()->getLockValue(),
            dtor_nodes.front()->getLockValue());
  EXPECT_EQ(critical_nodes.front()->getLockValue(),
            module->getNamedGlobal("crit"));
  EXPECT_EQ(end_critical_nodes.front()->getLockValue(),
            module->getNamedGlobal("crit"));
}

TEST_F(MHPAnalysisTest, LatchArriveAndWaitCreatesBarrierWaitNode) {
  const char *source = R"(
    declare void @fake_latch_arrive_and_wait(i8*)

    @latch = global i8 0

    define i32 @main() {
    entry:
      call void @fake_latch_arrive_and_wait(i8* @latch)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  const Instruction *call = nullptr;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      call = &inst;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto nodes = tfg.getNodes(call);
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes.front()->getType(), SyncNodeType::BARRIER_WAIT);
}

TEST_F(MHPAnalysisTest, OpenMPTaskBodyMustPrecedeTaskwaitContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

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
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
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

  EXPECT_TRUE(hb.mustPrecede(task_store, after));
  EXPECT_FALSE(mhp.mayHappenInParallel(task_store, after));
}

TEST_F(MHPAnalysisTest, JoinStatistics) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
  EXPECT_GE(stats.num_joins, 1);
}

TEST_F(MHPAnalysisTest, RecursiveCallGraphDoesNotExplodeContexts) {
  const char *source = R"(
    define void @recur(i32 %n) {
    entry:
      %stop = icmp eq i32 %n, 0
      br i1 %stop, label %exit, label %step

    step:
      %next = sub i32 %n, 1
      call void @recur(i32 %next)
      br label %exit

    exit:
      ret void
    }

    define i32 @main() {
    entry:
      call void @recur(i32 2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  EXPECT_NO_THROW(mhp.analyze());
}

TEST_F(MHPAnalysisTest, ThreadFlowGraphNodes) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i8* @worker(i8* %arg) {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto forkNodes = tfg.getNodesOfType(SyncNodeType::THREAD_FORK);
  auto joinNodes = tfg.getNodesOfType(SyncNodeType::THREAD_JOIN);
  auto lockNodes = tfg.getNodesOfType(SyncNodeType::LOCK_ACQUIRE);
  auto unlockNodes = tfg.getNodesOfType(SyncNodeType::LOCK_RELEASE);

  EXPECT_GE(forkNodes.size(), 1u);
  EXPECT_GE(joinNodes.size(), 1u);
  EXPECT_GE(lockNodes.size(), 1u);
  EXPECT_GE(unlockNodes.size(), 1u);
}

TEST_F(MHPAnalysisTest, ForkJoinOrdering) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
      %w1 = add i32 40, 2
      %w2 = add i32 %w1, 1
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %pre = add i32 1, 2
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %mid = add i32 3, 4
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *pre = findInstructionByName(*main_func, "pre");
  const Instruction *mid = findInstructionByName(*main_func, "mid");
  const Instruction *post = findInstructionByName(*main_func, "post");
  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  ASSERT_NE(pre, nullptr);
  ASSERT_NE(mid, nullptr);
  ASSERT_NE(post, nullptr);
  ASSERT_NE(w1, nullptr);

  EXPECT_TRUE(mhp.mustBeSequential(pre, w1));
  EXPECT_TRUE(mhp.mayHappenInParallel(mid, w1));
  EXPECT_TRUE(mhp.mustBeSequential(post, w1));
}

TEST_F(MHPAnalysisTest, ReusedThreadHandleStorageKeepsJoinAmbiguous) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      br i1 %cond, label %left, label %right

    left:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker1, i8* null)
      br label %join

    right:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker2, i8* null)
      br label %join

    join:
      call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *w1 = findInstructionByName(*module->getFunction("worker1"), "w1");
  const Instruction *w2 = findInstructionByName(*module->getFunction("worker2"), "w2");
  const Instruction *post = findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(w1, post));
  EXPECT_TRUE(mhp.mayHappenInParallel(w2, post));
}

TEST_F(MHPAnalysisTest, LoopForkDoesNotAutoSelfParallelizeWorkerBody) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
      %w1 = add i32 10, 20
      %w2 = add i32 %w1, 1
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %loop

    loop:
      %i = phi i32 [0, %entry], [%inc, %loop]
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %inc = add i32 %i, 1
      %cond = icmp slt i32 %inc, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);

  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  const Instruction *w2 = findInstructionByName(*worker_func, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(w1, w2));
}

TEST_F(MHPAnalysisTest, MutexSerializesCriticalSections) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i8* @worker(i8* %arg) {
      %wl = call i32 @pthread_mutex_lock(i8* @lock)
      %w_in = add i32 7, 8
      %wu = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %ml = call i32 @pthread_mutex_lock(i8* @lock)
      %m_in = add i32 1, 2
      %mu = call i32 @pthread_mutex_unlock(i8* @lock)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *m_in = findInstructionByName(*main_func, "m_in");
  const Instruction *w_in = findInstructionByName(*worker_func, "w_in");
  ASSERT_NE(m_in, nullptr);
  ASSERT_NE(w_in, nullptr);

  // Pure MHP tracks overlap only; the common mutex is a separate exclusion.
  auto *lockset = mhp.getLockSetAnalysis();
  ASSERT_NE(lockset, nullptr);
  auto stats = lockset->getStatistics();
  EXPECT_EQ(stats.num_locks, 1u);
  EXPECT_GE(stats.num_acquires, 2u);
  EXPECT_GE(stats.num_releases, 2u);
  EXPECT_TRUE(mhp.mayHappenInParallel(m_in, w_in));
}

TEST_F(MHPAnalysisTest, BarrierOrdersPreAndPostRegions) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_wait(i8*)

    @bar = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      %bw = call i32 @pthread_barrier_wait(i8* @bar)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %bw = call i32 @pthread_barrier_wait(i8* @bar)
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

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_shared = &writer_func->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader_func, "val");
  ASSERT_NE(load_shared, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}

TEST_F(MHPAnalysisTest, SplitPhaseBarrierOrdersPreAndPostRegions) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    @barrier = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 17, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
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

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}

TEST_F(MHPAnalysisTest, SplitPhaseBarrierArriveDoesNotOrderPostArriveCode) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    @barrier = global i8 0
    @lhs = global i32 0
    @rhs = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      call void @std_barrier_arriveEv(i8* @barrier)
      %after_arrive = load i32, i32* @lhs, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %before_wait = load i32, i32* @rhs, align 4
      call void @std_barrier_waitEv(i8* @barrier)
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

  const Instruction *after_arrive =
      findInstructionByName(*module->getFunction("writer"), "after_arrive");
  const Instruction *before_wait =
      findInstructionByName(*module->getFunction("reader"), "before_wait");
  ASSERT_NE(after_arrive, nullptr);
  ASSERT_NE(before_wait, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(mhp.mayHappenInParallel(after_arrive, before_wait));
  EXPECT_FALSE(hb.mustPrecede(after_arrive, before_wait));
  EXPECT_FALSE(hb.mustPrecede(before_wait, after_arrive));
}

TEST_F(MHPAnalysisTest, IncompleteBarrierPhaseDoesNotOrderPostBarrierCode) {
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

  const Instruction *writer_store =
      findInstructionByName(*module->getFunction("writer"), "writer_store");
  const Instruction *reader_load =
      findInstructionByName(*module->getFunction("reader"), "reader_load");
  ASSERT_NE(writer_store, nullptr);
  ASSERT_NE(reader_load, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(writer_store, reader_load));
  EXPECT_FALSE(hb.mustPrecede(writer_store, reader_load));
}

TEST_F(MHPAnalysisTest, CondSignalDoesNotCreateDefiniteHBToAllWaiters) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_cond_wait(i8*, i8*)
    declare i32 @pthread_cond_signal(i8*)

    @cond = global i8 0
    @mutex = global i8 0
    @shared = global i32 0

    define i8* @waiter1(i8* %arg) {
    entry:
      %w1 = call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %load1 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @waiter2(i8* %arg) {
    entry:
      %w2 = call i32 @pthread_cond_wait(i8* @cond, i8* @mutex)
      %load2 = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i8* @signaler(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      %sig = call i32 @pthread_cond_signal(i8* @cond)
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

  const Function *signaler_func = module->getFunction("signaler");
  const Function *waiter1_func = module->getFunction("waiter1");
  const Function *waiter2_func = module->getFunction("waiter2");
  ASSERT_NE(signaler_func, nullptr);
  ASSERT_NE(waiter1_func, nullptr);
  ASSERT_NE(waiter2_func, nullptr);

  const Instruction *store_shared = &signaler_func->getEntryBlock().front();
  const Instruction *load1 = findInstructionByName(*waiter1_func, "load1");
  const Instruction *load2 = findInstructionByName(*waiter2_func, "load2");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load1, nullptr);
  ASSERT_NE(load2, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_FALSE(hb.mustPrecede(store_shared, load1));
  EXPECT_FALSE(hb.mustPrecede(store_shared, load2));
}

TEST_F(MHPAnalysisTest, IncludedOpenMPTaskRunsInlineWithParentContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)

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
      call i32 @__kmpc_omp_task_begin_if0(i8* null, i32 0, i8* %task_raw)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *task_store =
      &module->getFunction("task_body")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(task_store, after));
  EXPECT_TRUE(mhp.mustBeSequential(task_store, after));
}

TEST_F(MHPAnalysisTest, JoinTargetThroughPhiResolves) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      br i1 %cond, label %left, label %right

    left:
      br label %join

    right:
      br label %join

    join:
      %phi_tid = phi i8* [ %tid, %left ], [ %tid, %right ]
      %joined = call i32 @pthread_join(i8* %phi_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mustBeSequential(worker_inst, post));
}

TEST_F(MHPAnalysisTest, JoinTargetThroughLoadRemainsNonMHP) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      %join_tid = bitcast i8* %tid to i8*
      call i32 @pthread_join(i8* %join_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mustBeSequential(worker_inst, post));
}

TEST_F(MHPAnalysisTest, MultiExitWorkerStillOrdersPostJoinContinuation) {
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
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(worker_func, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *left_work = findInstructionByName(*worker_func, "left_work");
  const Instruction *right_work =
      findInstructionByName(*worker_func, "right_work");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(left_work, nullptr);
  ASSERT_NE(right_work, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(left_work, post));
  EXPECT_FALSE(mhp.mayHappenInParallel(right_work, post));
  EXPECT_TRUE(mhp.mustBeSequential(left_work, post));
  EXPECT_TRUE(mhp.mustBeSequential(right_work, post));
}

TEST_F(MHPAnalysisTest, AmbiguousJoinDoesNotCreateDefiniteHB) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      %join_tid = select i1 %cond, i8* %tid1, i8* %tid2
      call i32 @pthread_join(i8* %join_tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker1 = module->getFunction("worker1");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker1, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *post = findInstructionByName(*main_func, "post");
  const Instruction *w1 = findInstructionByName(*worker1, "w1");
  const Instruction *w2 = findInstructionByName(*worker2, "w2");
  ASSERT_NE(post, nullptr);
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  EXPECT_FALSE(hb.mustPrecede(w1, post));
  EXPECT_FALSE(hb.mustPrecede(w2, post));
}

TEST_F(MHPAnalysisTest, ForeignJoinHandleDoesNotOrderLocalWorker) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main(i8* %foreign_tid) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %foreign_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_inst, post));
  EXPECT_FALSE(hb.mustPrecede(worker_inst, post));
}

TEST_F(MHPAnalysisTest, ReusedHandleJoinDoesNotOrderLaterCreatePhase) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker2, i8* null)
      %post = add i32 5, 6
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *w1 = findInstructionByName(*module->getFunction("worker1"), "w1");
  const Instruction *w2 = findInstructionByName(*module->getFunction("worker2"), "w2");
  const Instruction *post = findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(hb.mustPrecede(w1, post));
  EXPECT_TRUE(mhp.mayHappenInParallel(w2, post));
  EXPECT_FALSE(hb.mustPrecede(w2, post));
}

TEST_F(MHPAnalysisTest, RegionPartitionDoesNotOverlapAcrossBranchMerge) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      %lock_then = call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    else:
      %plain = add i32 1, 2
      br label %merge

    merge:
      %merge_val = phi i32 [ 1, %then ], [ %plain, %else ]
      ret i32 %merge_val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  size_t inst_count = 0;
  for (const BasicBlock &bb : *main_func) {
    for (const Instruction &inst : bb) {
      (void)inst;
      ++inst_count;
    }
  }

  size_t covered = 0;
  for (const auto &region : mhp.getThreadRegionAnalysis().getAllRegions()) {
    if (region->thread_id != 0) {
      continue;
    }
    covered += region->instructions.size();
  }

  EXPECT_EQ(covered, inst_count);
}

TEST_F(MHPAnalysisTest, HelperCalledBeforeAndAfterForkIsNotGloballyPrefork) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i32 @helper() {
    entry:
      %h = add i32 1, 2
      ret i32 %h
    }

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %pre = call i32 @helper()
      %tid = alloca i8
      %fork = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %post = call i32 @helper()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *helper_func = module->getFunction("helper");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(helper_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *helper_inst = findInstructionByName(*helper_func, "h");
  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  ASSERT_NE(helper_inst, nullptr);
  ASSERT_NE(worker_inst, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_EQ(mhp.getThreadFlowGraph().getNodes(helper_inst).size(), 2u);
  EXPECT_TRUE(mhp.mayHappenInParallel(helper_inst, worker_inst));
}

TEST_F(MHPAnalysisTest, HelperHiddenFirstForkDoesNotMakeCallerContinuationPrefork) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_helper(i8* %tid)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_store, after));
}

TEST_F(MHPAnalysisTest, HelperHiddenForkStillHonorsJoinOrderingAtCaller) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 9, i32* @shared, align 4
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_helper(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
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

  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(worker_store, after));
  EXPECT_TRUE(hb.mustPrecede(worker_store, after));
}

TEST_F(MHPAnalysisTest,
       RepeatedHelperForksOnSameHandleDoNotInventJoinOrdering) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 11, i32* @shared, align 4
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn_helper(i8* %tid)
      call void @spawn_helper(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
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

  const Instruction *worker_store =
      &module->getFunction("worker")->getEntryBlock().front();
  const Instruction *after =
      findInstructionByName(*module->getFunction("main"), "after");
  ASSERT_NE(worker_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(worker_store, after));
  EXPECT_FALSE(hb.mustPrecede(worker_store, after));
}

TEST_F(MHPAnalysisTest, LoopCreateJoinDoesNotAutoSelfParallelizeWorkerBody) {
  const char *source = R"(
    @x = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %a = add i32 1, 2
      %b = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);
  const Instruction *inst_a = findInstructionByName(*worker_func, "a");
  const Instruction *inst_b = findInstructionByName(*worker_func, "b");
  ASSERT_NE(inst_a, nullptr);
  ASSERT_NE(inst_b, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(inst_a, inst_b));
}

TEST_F(MHPAnalysisTest, OpenMPTargetDataBoundaryOrdersTaskContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end(i8*, i32)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 11, i32* @shared, align 4
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

  EXPECT_TRUE(hb.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}

TEST_F(MHPAnalysisTest, UnresolvedIndirectCallEnablesConservativeForkFallback) {
  const char *source = R"(
    @hook = external global void ()*

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define void @fork_helper() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %fn = load void ()*, void ()** @hook
      call void %fn()
      %post = add i32 3, 4
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker = module->getFunction("worker");
  ASSERT_NE(worker, nullptr);
  const Instruction *worker_inst = findInstructionByName(*worker, "w");
  ASSERT_NE(worker_inst, nullptr);

  EXPECT_EQ(mhp.getThreadID(worker_inst), std::numeric_limits<ThreadID>::max());
}

// Main function for tests
#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif

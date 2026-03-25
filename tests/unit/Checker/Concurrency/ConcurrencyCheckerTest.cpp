/**
 * @file ConcurrencyCheckerTest.cpp
 * @brief Unit tests for Concurrency checker
 *
 * The Concurrency checker detects concurrency-related bugs including:
 * - Data races
 * - Deadlocks
 * - Atomicity violations
 * - Lock mismatches
 */

#include "Checker/Concurrency/ConcurrencyChecker.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using namespace llvm;

class ConcurrencyCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source,
                                        "ConcurrencyCheckerTest");
  }
};

// Test 1: Lock acquire/release pattern
TEST_F(ConcurrencyCheckerTest, LockAcquireRelease) {
  const char *source = R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    
    define void @test_lock(i8* %mutex) {
      call void @pthread_mutex_lock(i8* %mutex)
      call void @pthread_mutex_unlock(i8* %mutex)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_lock");
  ASSERT_NE(F, nullptr);

  unsigned lockCount = 0, unlockCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *callee = CI->getCalledFunction()) {
          StringRef name = callee->getName();
          if (name == "pthread_mutex_lock")
            ++lockCount;
          else if (name == "pthread_mutex_unlock")
            ++unlockCount;
        }
      }
    }
  }

  EXPECT_EQ(lockCount, 1u);
  EXPECT_EQ(unlockCount, 1u);
}

// Test 2: Thread creation
TEST_F(ConcurrencyCheckerTest, ThreadCreation) {
  const char *source = R"(
    declare i8* @thread_func(i8*)
    declare i32 @pthread_create(i8**, i8*, i8* (i8*)*, i8*)
    
    define i32 @test_thread_create() {
      %thread = alloca i8*
      %result = call i32 @pthread_create(i8** %thread, i8* null, i8* (i8*)* @thread_func, i8* null)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *testFunc = module->getFunction("test_thread_create");
  Function *threadFunc = module->getFunction("thread_func");

  ASSERT_NE(testFunc, nullptr);
  ASSERT_NE(threadFunc, nullptr);

  CallInst *createCall = nullptr;
  for (auto &BB : *testFunc) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "pthread_create") {
          createCall = CI;
          break;
        }
      }
    }
  }

  ASSERT_NE(createCall, nullptr);
  EXPECT_EQ(createCall->arg_size(), 4u);
}

// Test 3: Shared variable access
TEST_F(ConcurrencyCheckerTest, SharedVariableAccess) {
  const char *source = R"(
    @shared_counter = global i32 0
    
    define void @increment() {
      %old = load i32, i32* @shared_counter
      %new = add i32 %old, 1
      store i32 %new, i32* @shared_counter
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  GlobalVariable *shared = module->getNamedGlobal("shared_counter");
  ASSERT_NE(shared, nullptr);

  Function *F = module->getFunction("increment");
  ASSERT_NE(F, nullptr);

  unsigned loadCount = 0, storeCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<LoadInst>(&I))
        ++loadCount;
      if (isa<StoreInst>(&I))
        ++storeCount;
    }
  }

  EXPECT_EQ(loadCount, 1u);
  EXPECT_EQ(storeCount, 1u);
}

// Test 4: Lock order inconsistency
TEST_F(ConcurrencyCheckerTest, LockOrderInconsistency) {
  const char *source = R"(
    declare void @pthread_mutex_lock(i8*)
    declare void @pthread_mutex_unlock(i8*)
    
    define void @func1(i8* %mutex1, i8* %mutex2) {
      call void @pthread_mutex_lock(i8* %mutex1)
      call void @pthread_mutex_lock(i8* %mutex2)
      call void @pthread_mutex_unlock(i8* %mutex2)
      call void @pthread_mutex_unlock(i8* %mutex1)
      ret void
    }
    
    define void @func2(i8* %mutex1, i8* %mutex2) {
      call void @pthread_mutex_lock(i8* %mutex2)
      call void @pthread_mutex_lock(i8* %mutex1)
      call void @pthread_mutex_unlock(i8* %mutex1)
      call void @pthread_mutex_unlock(i8* %mutex2)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *f1 = module->getFunction("func1");
  Function *f2 = module->getFunction("func2");

  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);

  bool f1HasLocks = false, f2HasLocks = false;
  for (auto *F : {f1, f2}) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (CI->getCalledFunction() &&
              CI->getCalledFunction()->getName() == "pthread_mutex_lock") {
            if (F == f1)
              f1HasLocks = true;
            else
              f2HasLocks = true;
          }
        }
      }
    }
  }

  EXPECT_TRUE(f1HasLocks);
  EXPECT_TRUE(f2HasLocks);
}

// Test 5: Atomic operation
TEST_F(ConcurrencyCheckerTest, AtomicOperation) {
  const char *source = R"(
    define i32 @atomic_fetch_add(i32* %ptr, i32 %val) {
      %result = atomicrmw add i32* %ptr, i32 %val monotonic
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("atomic_fetch_add");
  ASSERT_NE(F, nullptr);

  AtomicRMWInst *atomicrmw = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *ARMW = dyn_cast<AtomicRMWInst>(&I)) {
        atomicrmw = ARMW;
        break;
      }
    }
  }

  ASSERT_NE(atomicrmw, nullptr);
  EXPECT_EQ(atomicrmw->getOperation(), AtomicRMWInst::Add);
}

// Test 6: Thread join
TEST_F(ConcurrencyCheckerTest, ThreadJoin) {
  const char *source = R"(
    declare i32 @pthread_join(i8*, i8**)
    
    define i32 @test_join(i8* %thread) {
      %retval = alloca i8*
      %result = call i32 @pthread_join(i8* %thread, i8** %retval)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_join");
  ASSERT_NE(F, nullptr);

  CallInst *joinCall = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "pthread_join") {
          joinCall = CI;
          break;
        }
      }
    }
  }

  ASSERT_NE(joinCall, nullptr);
  EXPECT_EQ(joinCall->arg_size(), 2u);
}

// Test 7: Condition variable wait
TEST_F(ConcurrencyCheckerTest, ConditionVariable) {
  const char *source = R"(
    declare void @pthread_cond_wait(i8*, i8*)
    declare void @pthread_cond_signal(i8*)
    
    define void @test_cond_wait(i8* %cond, i8* %mutex) {
      call void @pthread_cond_wait(i8* %cond, i8* %mutex)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *waitFunc = module->getFunction("test_cond_wait");
  ASSERT_NE(waitFunc, nullptr);

  EXPECT_FALSE(waitFunc->empty());
}

// Test 8: Once initialization
TEST_F(ConcurrencyCheckerTest, OnceInitialization) {
  const char *source = R"(
    declare void @pthread_once(i8*, i8*)
    
    define void @init_func() {
      ret void
    }
    
    define void @test_once(i8* %once) {
      call void @pthread_once(i8* %once, i8* bitcast (void ()* @init_func to i8*))
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *testOnce = module->getFunction("test_once");
  Function *initFunc = module->getFunction("init_func");

  ASSERT_NE(testOnce, nullptr);
  ASSERT_NE(initFunc, nullptr);

  CallInst *onceCall = nullptr;
  for (auto &BB : *testOnce) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "pthread_once") {
          onceCall = CI;
          break;
        }
      }
    }
  }

  ASSERT_NE(onceCall, nullptr);
  EXPECT_EQ(onceCall->arg_size(), 2u);
}

// Test 9: Memory fence
TEST_F(ConcurrencyCheckerTest, MemoryFence) {
  const char *source = R"(
    declare void @llvm.arm.dmb(i8)
    
    define void @test_fence() {
      call void @llvm.arm.dmb(i8 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_fence");
  ASSERT_NE(F, nullptr);

  EXPECT_FALSE(F->empty());
}

// Test 10: Compare-and-swap
TEST_F(ConcurrencyCheckerTest, CompareAndSwap) {
  const char *source = R"(
    define void @test_cas(i32* %ptr) {
      %result = cmpxchg i32* %ptr, i32 0, i32 1 monotonic monotonic
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_cas");
  ASSERT_NE(F, nullptr);

  bool hasCmpXchg = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<AtomicCmpXchgInst>(&I)) {
        hasCmpXchg = true;
        break;
      }
    }
  }

  EXPECT_TRUE(hasCmpXchg);
}

TEST_F(ConcurrencyCheckerTest, DetectsOpenMPAtomicMismatch) {
  const char *source = R"(
    declare void @__kmpc_atomic_start()

    define void @test_openmp_atomic_mismatch() {
      call void @__kmpc_atomic_start()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OPENMP_ATOMIC_MISMATCH) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsOpenMPBarrierInCritical) {
  const char *source = R"(
    declare void @__kmpc_critical(i8*, i32, i8*)
    declare void @__kmpc_end_critical(i8*, i32, i8*)
    declare void @__kmpc_barrier(i8*, i32)

    define void @test_openmp_barrier_in_critical() {
      call void @__kmpc_critical(i8* null, i32 0, i8* null)
      call void @__kmpc_barrier(i8* null, i32 0)
      call void @__kmpc_end_critical(i8* null, i32 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OMP_BARRIER_IN_CRITICAL) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsOpenMPOrderedDependencyGap) {
  const char *source = R"(
    declare i32 @__kmpc_ordered(i8*, i32)
    declare void @__kmpc_end_ordered(i8*, i32)

    define void @test_openmp_ordered_gap() {
      %x = call i32 @__kmpc_ordered(i8* null, i32 0)
      call void @__kmpc_end_ordered(i8* null, i32 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OMP_ORDERED_DEPENDENCY) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsOpenMPIfFalseParallelPattern) {
  const char *source = R"(
    declare void @omp_set_num_threads(i32)
    declare void @__kmpc_fork_call(i8*, i32, i8*, ...)

    define void @test_openmp_if_false_parallel() {
      call void @omp_set_num_threads(i32 1)
      call void (i8*, i32, i8*, ...) @__kmpc_fork_call(i8* null, i32 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OMP_IF_FALSE_PARALLEL) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsOpenMPMissingSchedulePattern) {
  const char *source = R"(
    declare void @__kmpc_for_static_init_4(i8*, i32, i32*, i32*, i32*, i32*, i32, i32)

    define void @test_openmp_missing_schedule() {
      %last = alloca i32
      %lower = alloca i32
      %upper = alloca i32
      %stride = alloca i32
      call void @__kmpc_for_static_init_4(i8* null, i32 0, i32* %last,
                                          i32* %lower, i32* %upper,
                                          i32* %stride, i32 1, i32 1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OMP_MISSING_SCHEDULE) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, SuppressesPrivateInLoopWarningForSharedOnlyTask) {
  const char *source = R"(
    declare void @__kmpc_for_static_init_4(i8*, i32, i32*, i32*, i32*, i32*, i32, i32)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @.omp_outlined.(i32* %.omp.shared_ptr) {
    entry:
      %v = load i32, i32* %.omp.shared_ptr, align 4
      store i32 %v, i32* %.omp.shared_ptr, align 4
      ret void
    }

    define void @test_openmp_loop_shared_only_task() {
    entry:
      %last = alloca i32
      %lower = alloca i32
      %upper = alloca i32
      %stride = alloca i32
      call void @__kmpc_for_static_init_4(i8* null, i32 0, i32* %last,
                                          i32* %lower, i32* %upper,
                                          i32* %stride, i32 1, i32 1)
      %t = call i32 @__kmpc_omp_task(
          i8* null, i32 0,
          i8* bitcast (void (i32*)* @.omp_outlined. to i8*))
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType == concurrency::ConcurrencyBugType::OMP_PRIVATE_IN_LOOP) {
      found = true;
      break;
    }
  }
  EXPECT_FALSE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsPrivateInLoopWarningForPrivateLikeTask) {
  const char *source = R"(
    declare void @__kmpc_for_static_init_4(i8*, i32, i32*, i32*, i32*, i32*, i32, i32)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @.omp_outlined.(i32* %.omp.shared_ptr, i32 %.omp.val) {
    entry:
      %v = load i32, i32* %.omp.shared_ptr, align 4
      store i32 %v, i32* %.omp.shared_ptr, align 4
      %x = add i32 %.omp.val, 1
      ret void
    }

    define void @test_openmp_loop_private_like_task() {
    entry:
      %last = alloca i32
      %lower = alloca i32
      %upper = alloca i32
      %stride = alloca i32
      call void @__kmpc_for_static_init_4(i8* null, i32 0, i32* %last,
                                          i32* %lower, i32* %upper,
                                          i32* %stride, i32 1, i32 1)
      %t = call i32 @__kmpc_omp_task(
          i8* null, i32 0,
          i8* bitcast (void (i32*, i32)* @.omp_outlined. to i8*))
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::OpenMPChecker checker(*module, nullptr,
                                     ThreadAPI::getThreadAPI());
  auto reports = checker.checkOpenMPBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType == concurrency::ConcurrencyBugType::OMP_PRIVATE_IN_LOOP) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsMPIOrphanedRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8**)

    define void @test_mpi_orphan(i8* %buf, i8* %comm, i8** %req) {
      %call = call i32 @MPI_Isend(i8* %buf, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8** %req)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_ORPHANED_REQUEST) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, TracksOpenMPSummaryInCheckerStatistics) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare i32 @__kmpc_taskloop(i8*, i32, i8*, i32, i64*, i64, i32, i32, i64)
    declare void @__kmpc_taskgroup(i8*, i32)
    declare i32 @__kmpc_atomic_start()
    declare i32 @__kmpc_atomic_end()
    declare i32 @__kmpc_flush(i8*)

    define i32 @main() {
    entry:
      call void @__kmpc_taskgroup(i8* null, i32 0)
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 0, i8* null, i32 0, i8* null)
      call i32 @__kmpc_taskloop(i8* null, i32 0, i8* null, i32 0,
                                i64* null, i64 0, i32 0, i32 0, i64 0)
      call i32 @__kmpc_atomic_start()
      call i32 @__kmpc_atomic_end()
      call i32 @__kmpc_flush(i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableDataRaceCheck(false);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableMPICheck(false);
  checker.enableOpenMPCheck(true);
  checker.runAnalyses();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.openMPSummary.task_count, 2u);
  EXPECT_EQ(stats.openMPSummary.task_with_dependencies_count, 1u);
  EXPECT_EQ(stats.openMPSummary.taskloop_count, 1u);
  EXPECT_EQ(stats.openMPSummary.partial_wait_boundary_count, 1u);
  EXPECT_EQ(stats.openMPSummary.taskgroup_region_count, 1u);
  EXPECT_EQ(stats.openMPSummary.atomic_region_count, 1u);
  EXPECT_EQ(stats.openMPSummary.flush_count, 1u);
}

TEST_F(ConcurrencyCheckerTest, StaticVectorClockBackendRunsAnalyses) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.setMHPBackend(
      concurrency::ConcurrencyChecker::MHPBackendKind::StaticVectorClock);
  checker.enableOpenMPCheck(false);
  checker.enableMPICheck(false);
  EXPECT_NO_THROW(checker.runAnalyses());

  auto stats = checker.getStatistics();
  EXPECT_GT(stats.mhpPairs, 0u);
}

TEST_F(ConcurrencyCheckerTest, ConditionalMayLockDoesNotSuppressRealRace) {
  const char *source = R"(
    @lock = global i8 0
    @shared = global i32 0, align 4
    @flag = external global i1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %locked, label %merge

    locked:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    merge:
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %cond = load i1, i1* @flag
      br i1 %cond, label %locked, label %merge

    locked:
      call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    merge:
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest, ThreadLocalGlobalIsPrunedFromRaceCandidates) {
  const char *source = R"(
    @tls = thread_local global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      store i32 1, i32* @tls, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      store i32 2, i32* @tls, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  ThreadLocal::ThreadLocalAnalysis threadLocal(*module);
  threadLocal.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, &threadLocal, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       HelperSpawnedStackPayloadIsNotPrunedAsThreadLocal) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %typed = bitcast i8* %arg to i32*
      store i32 1, i32* %typed, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %typed = bitcast i8* %arg to i32*
      store i32 2, i32* %typed, align 4
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
      %shared_slot = alloca i32, align 4
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call void @spawn_helper(i8* %tid1, i32* %shared_slot,
                              i8* (i8*)* @worker1)
      call void @spawn_helper(i8* %tid2, i32* %shared_slot,
                              i8* (i8*)* @worker2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  ThreadLocal::ThreadLocalAnalysis threadLocal(*module);
  threadLocal.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, &threadLocal, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       LocalPointerCarrierPayloadIsStillReportedAsShared) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %typed = bitcast i8* %arg to i32*
      store i32 1, i32* %typed, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %typed = bitcast i8* %arg to i32*
      store i32 2, i32* %typed, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %shared_slot = alloca i32, align 4
      %carrier = alloca i32*, align 8
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      store i32* %shared_slot, i32** %carrier, align 8
      %payload1 = load i32*, i32** %carrier, align 8
      %raw1 = bitcast i32* %payload1 to i8*
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1,
                               i8* %raw1)
      %payload2 = load i32*, i32** %carrier, align 8
      %raw2 = bitcast i32* %payload2 to i8*
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2,
                               i8* %raw2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  ThreadLocal::ThreadLocalAnalysis threadLocal(*module);
  threadLocal.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, &threadLocal, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       ReaderWriterLockReadWritePairStillReportsRace) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @lock = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_rwlock_rdlock(i8*)
    declare i32 @pthread_rwlock_wrlock(i8*)

    define i8* @reader(i8* %arg) {
    entry:
      call i32 @pthread_rwlock_rdlock(i8* @lock)
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @writer(i8* %arg) {
    entry:
      call i32 @pthread_rwlock_wrlock(i8* @lock)
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @reader, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @writer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       CountingSemaphoreDoesNotSuppressEndToEndRaceReports) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @sem = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @sem_wait(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call i32 @sem_wait(i8* @sem)
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call i32 @sem_wait(i8* @sem)
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       BinarySemaphoreStillSuppressesEndToEndRaceReports) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @sem = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @binary_sem_wait(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call i32 @binary_sem_wait(i8* @sem)
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call i32 @binary_sem_wait(i8* @sem)
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest, FenceWithoutConcreteWitnessDoesNotSuppressRace) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
      store atomic i32 1, i32* @flag release, align 4
      fence release
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      fence acquire
      %seen = load atomic i32, i32* @flag acquire, align 4
      store i32 %seen, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  lotus::HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), &hb);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       ReleaseAcquireMessagePassingSuppressesPayloadRace) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @flag = global i32 0, align 4

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
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %loaded = load i32, i32* @shared, align 4
      store i32 %loaded, i32* @shared, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  lotus::HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), &hb);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (const auto *store = dyn_cast<StoreInst>(&inst)) {
      if (store->getPointerOperand() == module->getNamedGlobal("shared")) {
        store2 = &inst;
      }
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       LaterNonReleaseStoreDoesNotSuppressRaceThroughReleaseHB) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 1, i32* @shared, align 4
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
      br i1 %ready, label %write, label %exit

    write:
      store i32 %seen, i32* @shared, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      %tid3 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @overwriter, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  lotus::HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), &hb);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       InitialAtomicValueWitnessDoesNotSuppressPayloadRace) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @flag = global i32 1, align 4

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
      %ready = icmp eq i32 %seen, 1
      br i1 %ready, label %write, label %exit

    write:
      store i32 %seen, i32* @shared, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  lotus::HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), &hb);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("writer"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("reader"))) {
    if (const auto *store = dyn_cast<StoreInst>(&inst)) {
      if (store->getPointerOperand() == module->getNamedGlobal("shared")) {
        store2 = &inst;
      }
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       PthreadGetspecificDerivedPointersAreNotPrunedAsThreadLocal) {
  const char *source = R"(
    declare i8* @pthread_getspecific(i32)
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %tls = call i8* @pthread_getspecific(i32 0)
      %typed = bitcast i8* %tls to i32*
      store i32 1, i32* %typed, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %tls = call i8* @pthread_getspecific(i32 0)
      %typed = bitcast i8* %tls to i32*
      store i32 2, i32* %typed, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();
  ThreadLocal::ThreadLocalAnalysis threadLocal(*module);
  threadLocal.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, &threadLocal, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest,
       UnresolvedIndirectCallDoesNotPreserveOptimisticLockSuppression) {
  const char *source = R"(
    @hook = external global void ()*
    @lock = global i8 0
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    define void @unlock_helper() {
    entry:
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret void
    }

    define i8* @worker1(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = load void ()*, void ()** @hook
      call void %fn()
      store i32 1, i32* @shared, align 4
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call i32 @pthread_mutex_lock(i8* @lock)
      %fn = load void ()*, void ()** @hook
      call void %fn()
      store i32 2, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_TRUE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest, SuppressesRaceForOpenMPPrivateLikeCaptures) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @task1(i32* %.omp.private_buf) {
    entry:
      %tmp = alloca i32*, align 8
      store i32* %.omp.private_buf, i32** %tmp, align 8
      %loaded = load i32*, i32** %tmp, align 8
      %elt = getelementptr inbounds i32, i32* %loaded, i64 1
      store i32 1, i32* %elt, align 4
      ret void
    }

    define internal void @task2(i32* %.omp.private_buf) {
    entry:
      %tmp = alloca i32*, align 8
      store i32* %.omp.private_buf, i32** %tmp, align 8
      %loaded = load i32*, i32** %tmp, align 8
      %elt = getelementptr inbounds i32, i32* %loaded, i64 1
      store i32 2, i32* %elt, align 4
      ret void
    }

    define i32 @main() {
    entry:
      %t1 = call i32 @__kmpc_omp_task(
          i8* null, i32 0, i8* bitcast (void (i32*)* @task1 to i8*))
      %t2 = call i32 @__kmpc_omp_task(
          i8* null, i32 0, i8* bitcast (void (i32*)* @task2 to i8*))
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst : instructions(*module->getFunction("task1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst : instructions(*module->getFunction("task2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest, SuppressesRaceInsideNamedOpenMPCriticalSection) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @crit = global [8 x i32] zeroinitializer

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define i8* @worker1(i8* %arg) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      store i32 1, i32* @shared, align 4
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      store i32 2, i32* @shared, align 4
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  mhp::MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  lotus::EscapeAnalysis escape(*module);
  escape.analyze();

  concurrency::DataRaceChecker checker(*module, &mhp, mhp.getLockSetAnalysis(),
                                       &escape, nullptr, nullptr,
                                       mhp.getAliasAnalysis(), nullptr);

  const Instruction *store1 = nullptr;
  const Instruction *store2 = nullptr;
  for (const Instruction &inst :
       instructions(*module->getFunction("worker1"))) {
    if (isa<StoreInst>(&inst)) {
      store1 = &inst;
      break;
    }
  }
  for (const Instruction &inst :
       instructions(*module->getFunction("worker2"))) {
    if (isa<StoreInst>(&inst)) {
      store2 = &inst;
      break;
    }
  }

  ASSERT_NE(store1, nullptr);
  ASSERT_NE(store2, nullptr);
  EXPECT_FALSE(checker.wouldReportDataRace(store1, store2));
}

TEST_F(ConcurrencyCheckerTest, TracksMPISummaryInCheckerStatistics) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      call i32 @MPI_Request_free(i8* %req)
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableDataRaceCheck(false);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableOpenMPCheck(false);
  checker.enableMPICheck(true);
  checker.runAnalyses();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.mpiSummary.operation_count, 7u);
  EXPECT_EQ(stats.mpiSummary.nonblocking_operation_count, 2u);
  EXPECT_EQ(stats.mpiSummary.collective_operation_count, 1u);
  EXPECT_EQ(stats.mpiSummary.request_management_count, 1u);
  EXPECT_EQ(stats.mpiSummary.rma_operation_count, 1u);
  EXPECT_EQ(stats.mpiSummary.rma_sync_count, 2u);
  EXPECT_EQ(stats.mpiSummary.leaked_window_count, 1u);
  EXPECT_EQ(stats.mpiSummary.collective_slot_count, 1u);
}

TEST_F(ConcurrencyCheckerTest, DetectsMPIInvalidTagBug) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define void @test_mpi_invalid_tag(i8* %buf, i8* %comm) {
      %call = call i32 @MPI_Send(i8* %buf, i32 1, i32 0, i32 1, i32 -1, i8* %comm)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found = false;
  for (const auto &report : reports) {
    if (report.bugType == concurrency::ConcurrencyBugType::MPI_INVALID_TAG) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(ConcurrencyCheckerTest, DetectsMPIWindowLifecycleBugs) {
  const char *source = R"(
    @win = global i8 0, align 1

    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_free(i8* @win)
      call i32 @MPI_Win_free(i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found_invalid_transition = false;
  bool found_use_after_free = false;
  bool found_double_free = false;
  for (const auto &report : reports) {
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_INVALID_RMA_TRANSITION) {
      found_invalid_transition = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_USE_AFTER_FREE_WINDOW) {
      found_use_after_free = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_DOUBLE_WINDOW_FREE) {
      found_double_free = true;
    }
  }

  EXPECT_TRUE(found_invalid_transition);
  EXPECT_TRUE(found_use_after_free);
  EXPECT_TRUE(found_double_free);
}

TEST_F(ConcurrencyCheckerTest, DetectsAdditionalMPIMappingBugs) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Comm_free(i8*)

    @MPI_IN_PLACE = external global i8
    @MPI_COMM_NULL = external global i8

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -3, i32 7, i8* %comm)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 2, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      call i32 @MPI_Bcast(i8* @MPI_IN_PLACE, i32 1, i32 0, i32 -1, i8* %comm)
      call i32 @MPI_Comm_free(i8* @MPI_COMM_NULL)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::MPIChecker checker(*module);
  auto reports = checker.checkMPIBugs();

  bool found_invalid_rank = false;
  bool found_type_size_mismatch = false;
  bool found_destroy_null_comm = false;
  bool found_request_free_after_wait = false;
  bool found_in_place_wrong_op = false;

  for (const auto &report : reports) {
    if (report.bugType == concurrency::ConcurrencyBugType::MPI_INVALID_RANK) {
      found_invalid_rank = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_TYPE_SIZE_MISMATCH) {
      found_type_size_mismatch = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_DESTROY_NULL_COMM) {
      found_destroy_null_comm = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_REQUEST_FREE_AFTER_WAIT) {
      found_request_free_after_wait = true;
    }
    if (report.bugType ==
        concurrency::ConcurrencyBugType::MPI_IN_PLACE_WRONG_OP) {
      found_in_place_wrong_op = true;
    }
  }

  EXPECT_TRUE(found_invalid_rank);
  EXPECT_TRUE(found_type_size_mismatch);
  EXPECT_TRUE(found_destroy_null_comm);
  EXPECT_TRUE(found_request_free_after_wait);
  EXPECT_TRUE(found_in_place_wrong_op);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "ConcurrencyCheckerTestSupport.h"

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
TEST_F(ConcurrencyCheckerTest,
       SuppressesPrivateInLoopWarningForSharedOnlyTask) {
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
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OMP_PRIVATE_IN_LOOP) {
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
    if (report.bugType ==
        concurrency::ConcurrencyBugType::OMP_PRIVATE_IN_LOOP) {
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

TEST_F(ConcurrencyCheckerTest, MultiStageSparseRefinementRunsOnRacePath) {
  auto module = parseModule(R"(
    @shared = global i8* null
    @x = global i8 0
    @y = global i8 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8**)

    define i8* @writer_x(i8* %arg) {
    entry:
      store i8* @x, i8** @shared
      ret i8* null
    }
    define i8* @writer_y(i8* %arg) {
    entry:
      store i8* @y, i8** @shared
      ret i8* null
    }
    define i32 @main() {
    entry:
      %t1 = alloca i8
      %t2 = alloca i8
      call i32 @pthread_create(i8* %t1, i8* null,
                               i8* (i8*)* @writer_x, i8* null)
      call i32 @pthread_create(i8* %t2, i8* null,
                               i8* (i8*)* @writer_y, i8* null)
      call i32 @pthread_join(i8* %t1, i8** null)
      call i32 @pthread_join(i8* %t2, i8** null)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableMultiStageSlicing(true);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableOpenMPCheck(false);
  checker.enableMPICheck(false);
  checker.enableCUDACheck(false);
  EXPECT_NO_THROW(checker.runAnalyses());

  const auto stats = checker.getStatistics();
  EXPECT_GT(stats.sparseInterferenceEdges, 0u);
  EXPECT_GT(stats.sparsePointsToFacts, 0u);
  EXPECT_GT(stats.sparseMemoryRegions, 0u);
  EXPECT_GT(stats.sparseOriginalNodes, stats.sparseSlicedNodes);
  EXPECT_EQ(stats.sparsePreThreads, 3u);
  EXPECT_EQ(stats.sparseMainThreads, 3u);
  EXPECT_GT(stats.sparseJoinMemoryEdges, 0u);
}

TEST_F(ConcurrencyCheckerTest, CUDAStatisticsAreCollected) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaDeviceSynchronize()
    declare void @llvm.nvvm.barrier0()
    declare void @llvm.nvvm.bar.warp.sync(i32)
    declare void @llvm.nvvm.membar.gl()
    declare i32 @atomicAdd(i32*, i32)

    define void @kernel(i32* %ptr) {
    entry:
      call void @llvm.nvvm.barrier0()
      call void @llvm.nvvm.bar.warp.sync(i32 -1)
      call void @llvm.nvvm.membar.gl()
      %old = call i32 @atomicAdd(i32* %ptr, i32 1)
      ret void
    }

    define i32 @main(i32* %ptr) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel(i32* %ptr)
      %sync = call i32 @cudaDeviceSynchronize()
      ret i32 %sync
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
  checker.enableMPICheck(false);
  checker.enableCUDACheck(true);
  checker.runAnalyses();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.cudaSummary.kernel_launch_count, 1u);
  EXPECT_EQ(stats.cudaSummary.device_sync_count, 1u);
  EXPECT_EQ(stats.cudaSummary.barrier_count, 1u);
  EXPECT_EQ(stats.cudaSummary.warp_barrier_count, 1u);
  EXPECT_EQ(stats.cudaSummary.memory_barrier_count, 1u);
  EXPECT_EQ(stats.cudaSummary.atomic_count, 1u);
}
TEST_F(ConcurrencyCheckerTest, CUDADeviceSynchronizeSuppressesPostKernelRace) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaDeviceSynchronize()

    define void @kernel() {
    entry:
      store i32 1, i32* @shared, align 4
      ret void
    }

    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      %sync = call i32 @cudaDeviceSynchronize()
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::ConcurrencyChecker checker(*module);
  checker.enableDeadlockCheck(false);
  checker.enableAtomicityCheck(false);
  checker.enableCondVarCheck(false);
  checker.enableLockMismatchCheck(false);
  checker.enableOpenMPCheck(false);
  checker.enableMPICheck(false);
  checker.enableCUDACheck(true);
  checker.runAnalyses();
  checker.checkDataRaces();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.dataRacesFound, 0u);
}
TEST_F(ConcurrencyCheckerTest, CUDAAnalysisReportsPerformanceRisks) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [128 x i32] zeroinitializer
    @global_arr = addrspace(1) global [128 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %cond = icmp eq i32 %tid, 0
      br i1 %cond, label %then, label %else

    then:
      br label %merge

    else:
      br label %merge

    merge:
      %mul = mul i32 %tid, 2
      %shared_idx = getelementptr [128 x i32], [128 x i32] addrspace(3)* @shared_arr, i32 0, i32 %mul
      store i32 %tid, i32 addrspace(3)* %shared_idx
      %global_idx = getelementptr [128 x i32], [128 x i32] addrspace(1)* @global_arr, i32 0, i32 %mul
      %val = load i32, i32 addrspace(1)* %global_idx
      ret void
    }

    define void @main(i32 %sym_grid) {
    entry:
      call void @__set_CUDAConfig(i32 %sym_grid, i32 32)
      call void @kernel()
      ret void
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
  checker.enableMPICheck(false);
  checker.enableCUDACheck(true);
  checker.runAnalyses();
  checker.checkCUDABugs();

  auto stats = checker.getStatistics();
  EXPECT_GT(stats.cudaSummary.warp_divergence_count, 0u);
  EXPECT_GT(stats.cudaSummary.bank_conflict_count, 0u);
  EXPECT_GT(stats.cudaSummary.uncoalesced_access_count, 0u);
  EXPECT_GT(stats.cudaBugsFound, 0u);
}
TEST_F(ConcurrencyCheckerTest,
       CUDACheckerAvoidsThreadPrivateSharedFalsePositive) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %slot = getelementptr [64 x i32], [64 x i32] addrspace(3)* @shared_arr, i32 0, i32 %tid
      store volatile i32 %tid, i32 addrspace(3)* %slot
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
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
  checker.enableMPICheck(false);
  checker.enableCUDACheck(true);
  checker.runAnalyses();
  checker.checkCUDABugs();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.cudaSummary.shared_race_count, 0u);
  EXPECT_EQ(stats.cudaSummary.bank_conflict_count, 0u);
  EXPECT_EQ(stats.cudaSummary.volatile_missing_count, 0u);
}

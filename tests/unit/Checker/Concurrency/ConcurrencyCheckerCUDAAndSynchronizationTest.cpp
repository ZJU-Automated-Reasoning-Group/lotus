#include "ConcurrencyCheckerTestSupport.h"

TEST_F(ConcurrencyCheckerTest,
       CUDACheckerReportsBarrierMismatchAndCrossBlockRace) {
  const char *source = R"(
    @global_arr = addrspace(1) global [16 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
    declare void @llvm.nvvm.barrier0()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      %cond = icmp eq i32 %tid, 0
      br i1 %cond, label %then, label %merge

    then:
      call void @llvm.nvvm.barrier0()
      br label %merge

    merge:
      %slot = and i32 %bid, 1
      %ptr = getelementptr [16 x i32], [16 x i32] addrspace(1)* @global_arr, i32 0, i32 %slot
      store i32 %bid, i32 addrspace(1)* %ptr
      store i32 9, i32 addrspace(1)* %ptr
      ret void
    }

    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 4, i32 32)
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
  EXPECT_GT(stats.cudaSummary.barrier_mismatch_count, 0u);
  EXPECT_GT(stats.cudaSummary.global_race_count, 0u);
  EXPECT_GT(stats.cudaBugsFound, 0u);
}
TEST_F(ConcurrencyCheckerTest,
       CUDACheckerReportsParametricRaceAndMemoryAmbiguity) {
  const char *source = R"(
    @shared_arr = addrspace(3) global [32 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel(i32* %mystery) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %slot = and i32 %tid, 1
      %shared_idx = getelementptr [32 x i32], [32 x i32] addrspace(3)* @shared_arr, i32 0, i32 %slot
      store i32 %tid, i32 addrspace(3)* %shared_idx
      store i32 1, i32 addrspace(3)* %shared_idx
      %ptr = getelementptr i32, i32* %mystery, i32 %slot
      store i32 %tid, i32* %ptr
      store i32 2, i32* %ptr
      ret void
    }

    define void @main(i32 %sym_block, i32* %mystery) {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 %sym_block)
      call void @kernel(i32* %mystery)
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
  EXPECT_GT(stats.cudaSummary.shared_race_count, 0u);
  EXPECT_GT(stats.cudaBugsFound, 1u);
}
TEST_F(ConcurrencyCheckerTest, CUDACheckerSuppressesOrderedInterKernelHazard) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaDeviceSynchronize()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_producer()
      %sync = call i32 @cudaDeviceSynchronize()
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel_consumer()
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
  checker.checkCUDABugs();

  auto stats = checker.getStatistics();
  EXPECT_EQ(stats.cudaSummary.global_race_count, 0u);
  EXPECT_EQ(stats.cudaSummary.inter_kernel_hazard_count, 0u);
}
TEST_F(ConcurrencyCheckerTest, CUDACheckerReportsUnorderedInterKernelHazard) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque

    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64,
                                  i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel_producer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s1)
      call void @kernel_producer()
      %l1 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel_consumer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s2)
      call void @kernel_consumer()
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
  EXPECT_EQ(stats.cudaSummary.inter_kernel_hazard_count, 1u);
  EXPECT_GT(stats.cudaBugsFound, 0u);
}
TEST_F(ConcurrencyCheckerTest, CUDACheckerSummarizesUnifiedMemoryOperations) {
  const char *source = R"(
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaMallocHost(i8**, i64)
    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, i8*)

    define i32 @main() {
    entry:
      %managed = alloca i8*
      %host = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 64, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %p = call i32 @cudaMemPrefetchAsync(i8* %managed_ptr, i64 64, i32 2, i8* null)
      %h = call i32 @cudaMallocHost(i8** %host, i64 32)
      %sum = add i32 %m, %p
      %sum2 = add i32 %sum, %h
      ret i32 %sum2
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
  EXPECT_EQ(stats.cudaSummary.unified_memory_count, 3u);
  EXPECT_EQ(stats.cudaSummary.managed_allocation_count, 1u);
  EXPECT_EQ(stats.cudaSummary.unified_prefetch_count, 1u);
  EXPECT_EQ(stats.cudaSummary.unified_host_allocation_count, 1u);
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
TEST_F(ConcurrencyCheckerTest, ReaderWriterLockReadWritePairStillReportsRace) {
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

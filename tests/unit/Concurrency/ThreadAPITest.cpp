#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;

class ThreadAPITest : public lotus::unittest::LlvmModuleTest {};

TEST_F(ThreadAPITest, ParsesExtendedTypeNames) {
  EXPECT_EQ(ThreadAPI::stringToType("TD_CANCEL"), ThreadAPI::TD_CANCEL);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_BARRIER"),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(ThreadAPI::stringToType("TD_SHARED_LOCK_DTOR"),
            ThreadAPI::TD_SHARED_LOCK_DTOR);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_TASKWAIT_DEPS"),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_SINGLE_END"),
            ThreadAPI::TD_OMP_SINGLE_END);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_ORDERED_START"),
            ThreadAPI::TD_OMP_ORDERED_START);
  EXPECT_EQ(ThreadAPI::stringToType("TD_OMP_TARGET_DATA_UPDATE"),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_PERSISTENT_SEND_INIT"),
            ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_PERSISTENT_RECV_INIT"),
            ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT);
  EXPECT_EQ(ThreadAPI::stringToType("TD_MPI_REQUEST_START"),
            ThreadAPI::TD_MPI_REQUEST_START);
}

TEST_F(ThreadAPITest, PthreadCancelIsNotClassifiedAsJoin) {
  const char *source = R"(
    declare i32 @pthread_cancel(i8*)

    define i32 @main(i8* %tid) {
    entry:
      %cancel = call i32 @pthread_cancel(i8* %tid)
      ret i32 %cancel
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *cancel_func = module->getFunction("pthread_cancel");
  ASSERT_NE(cancel_func, nullptr);
  EXPECT_EQ(api->getType(cancel_func), ThreadAPI::TD_CANCEL);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *cancel_call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDJoin(cancel_call));
}

TEST_F(ThreadAPITest, DistinguishesBlockingAndNonBlockingMPICollectives) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %bar = call i32 @MPI_Barrier(i8* %comm)
      %ibar = call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      %bcast = call i32 @MPI_Bcast(i8* null, i32 0, i32 0, i32 0, i8* %comm)
      %ibcast = call i32 @MPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *bar = &*it++;
  const Instruction *ibar = &*it++;
  const Instruction *bcast = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_TRUE(api->isBlockingMPIBarrier(bar));
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isBlockingMPICollective(bcast));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}

TEST_F(ThreadAPITest, NormalizesPMPIAliasesForMPIClassification) {
  const char *source = R"(
    declare i32 @PMPI_Ibarrier(i8*, i8*)
    declare i32 @PMPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %req) {
    entry:
      %ibar = call i32 @PMPI_Ibarrier(i8* %comm, i8* %req)
      %ibcast = call i32 @PMPI_Ibcast(i8* null, i32 0, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *ibar = &*it++;
  const Instruction *ibcast = &*it++;

  EXPECT_EQ(api->getType(module->getFunction("PMPI_Ibarrier")),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("PMPI_Ibcast")),
            ThreadAPI::TD_MPI_BCAST);
  EXPECT_TRUE(api->isNonBlockingMPIBarrier(ibar));
  EXPECT_TRUE(api->isNonBlockingMPICollective(ibcast));
}

TEST_F(ThreadAPITest, MatchesSpecificOpenMPTargetDataBeforeGenericTarget) {
  const char *source = R"(
    declare void @__tgt_target_data_begin(i64, i8*)
    declare void @__tgt_target_data_end(i64, i8*)

    define void @main() {
    entry:
      call void @__tgt_target_data_begin(i64 0, i8* null)
      call void @__tgt_target_data_end(i64 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_begin")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_end")),
            ThreadAPI::TD_OMP_TARGET_DATA_END);
}

TEST_F(ThreadAPITest, PreservesMangledCppAsyncNamesDuringClassification) {
  const char *source = R"(
    declare void @_ZNSt5async12launch_asyncEv(i32)

    define void @main() {
    entry:
      call void @_ZNSt5async12launch_asyncEv(i32 1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *async_func = module->getFunction("_ZNSt5async12launch_asyncEv");
  ASSERT_NE(async_func, nullptr);
  EXPECT_EQ(api->getType(async_func), ThreadAPI::TD_ASYNC);
}

TEST_F(ThreadAPITest, AsyncWithUnknownPolicyStillLooksForkLike) {
  const char *source = R"(
    declare void @_ZNSt5async12launch_asyncEiPFvPvES1_(i32, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @main(i32 %policy) {
    entry:
      %payload = alloca i8, align 1
      call void @_ZNSt5async12launch_asyncEiPFvPvES1_(
          i32 %policy, i8* (i8*)* @worker, i8* %payload)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = main_func->getEntryBlock().getTerminator()->getPrevNode();
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(api->isTDFork(call));
  EXPECT_EQ(api->getForkedFun(call)->stripPointerCasts(),
            module->getFunction("worker"));
  auto payloads = api->getForkPayloadArgs(call);
  ASSERT_EQ(payloads.size(), 1u);
  EXPECT_EQ(payloads[0], &main_func->getEntryBlock().front());
}

TEST_F(ThreadAPITest, FunctorStyleThreadLaunchStillReturnsPayloadArgs) {
  const char *source = R"(
    declare void @_ZNSt6threadC1ER8FunctorPi(i8*, i8*, i32*)

    define void @main() {
    entry:
      %thread_obj = alloca i8, align 1
      %functor = alloca i8, align 1
      %payload = alloca i32, align 4
      call void @_ZNSt6threadC1ER8FunctorPi(i8* %thread_obj, i8* %functor,
                                            i32* %payload)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *thread_obj = &*it++;
  const Instruction *functor = &*it++;
  const Instruction *payload = &*it++;
  const Instruction *call = &*it;
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(api->isTDFork(call));
  EXPECT_EQ(api->getForkedFun(call), nullptr);
  auto payloads = api->getForkPayloadArgs(call);
  ASSERT_EQ(payloads.size(), 2u);
  EXPECT_EQ(payloads[0], functor);
  EXPECT_EQ(payloads[1], payload);
  EXPECT_NE(thread_obj, nullptr);
}

TEST_F(ThreadAPITest, RecognizesExtendedOpenMPTargetDataVariantsAndHelpers) {
  const char *source = R"(
    declare void @__tgt_target_data_update(i64, i8*)
    declare void @__tgt_target_enter_data(i64, i8*)
    declare void @__tgt_target_exit_data(i64, i8*)

    define void @main() {
    entry:
      call void @__tgt_target_data_update(i64 0, i8* null)
      call void @__tgt_target_enter_data(i64 0, i8* null)
      call void @__tgt_target_exit_data(i64 0, i8* null)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_data_update")),
            ThreadAPI::TD_OMP_TARGET_DATA_UPDATE);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_enter_data")),
            ThreadAPI::TD_OMP_TARGET_DATA_BEGIN);
  EXPECT_EQ(api->getType(module->getFunction("__tgt_target_exit_data")),
            ThreadAPI::TD_OMP_TARGET_DATA_END);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *update = &*it++;
  const Instruction *enter = &*it++;
  const Instruction *exit = &*it++;

  EXPECT_TRUE(api->isOMPTargetOp(update));
  EXPECT_TRUE(api->isOMPTargetDataOp(update));
  EXPECT_TRUE(api->isOMPTargetDataOp(enter));
  EXPECT_TRUE(api->isOMPTargetDataOp(exit));
}

TEST_F(ThreadAPITest, RecognizesOpenMPLockLifecycleAndTryLockRoutines) {
  const char *source = R"(
    declare void @omp_init_lock(i8*)
    declare i32 @omp_test_lock(i8*)
    declare void @omp_destroy_lock(i8*)
    declare void @omp_init_nest_lock(i8*)
    declare i32 @omp_test_nest_lock(i8*)
    declare void @omp_destroy_nest_lock(i8*)

    define void @main(i8* %lock, i8* %nest) {
    entry:
      call void @omp_init_lock(i8* %lock)
      call i32 @omp_test_lock(i8* %lock)
      call void @omp_destroy_lock(i8* %lock)
      call void @omp_init_nest_lock(i8* %nest)
      call i32 @omp_test_nest_lock(i8* %nest)
      call void @omp_destroy_nest_lock(i8* %nest)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("omp_init_lock")),
            ThreadAPI::TD_MUTEX_INI);
  EXPECT_EQ(api->getType(module->getFunction("omp_test_lock")),
            ThreadAPI::TD_TRY_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("omp_destroy_lock")),
            ThreadAPI::TD_MUTEX_DESTROY);
  EXPECT_EQ(api->getType(module->getFunction("omp_init_nest_lock")),
            ThreadAPI::TD_MUTEX_INI);
  EXPECT_EQ(api->getType(module->getFunction("omp_test_nest_lock")),
            ThreadAPI::TD_TRY_ACQUIRE);
  EXPECT_EQ(api->getType(module->getFunction("omp_destroy_nest_lock")),
            ThreadAPI::TD_MUTEX_DESTROY);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  ++it;
  const Instruction *test_lock = &*it++;
  ++it;
  ++it;
  const Instruction *test_nest_lock = &*it;
  EXPECT_TRUE(api->isTryLock(test_lock));
  EXPECT_TRUE(api->isTryLock(test_nest_lock));
}

TEST_F(ThreadAPITest, DefaultSemaphoresAreNotLockExclusionOps) {
  const char *source = R"(
    declare i32 @sem_wait(i8*)
    declare i32 @sem_post(i8*)
    declare void @fake_counting_semaphore_acquireEv(i8*)
    declare void @fake_counting_semaphore_releaseEv(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @sem_wait(i8* %sem)
      call i32 @sem_post(i8* %sem)
      call void @fake_counting_semaphore_acquireEv(i8* %sem)
      call void @fake_counting_semaphore_releaseEv(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *sem_wait_call = &*it++;
  const Instruction *sem_post_call = &*it++;
  const Instruction *cpp_acquire = &*it++;
  const Instruction *cpp_release = &*it++;

  EXPECT_TRUE(api->isSemaphoreOp(sem_wait_call));
  EXPECT_TRUE(api->isSemaphoreOp(sem_post_call));
  EXPECT_TRUE(api->isSemaphoreOp(cpp_acquire));
  EXPECT_TRUE(api->isSemaphoreOp(cpp_release));

  EXPECT_FALSE(api->isBinarySemaphoreOp(sem_wait_call));
  EXPECT_FALSE(api->isBinarySemaphoreOp(cpp_acquire));
  EXPECT_FALSE(api->isTDAcquire(sem_wait_call));
  EXPECT_FALSE(api->isTDRelease(sem_post_call));
  EXPECT_FALSE(api->isTDAcquire(cpp_acquire));
  EXPECT_FALSE(api->isTDRelease(cpp_release));
}

TEST_F(ThreadAPITest, ConfigTaggedBinarySemaphoresRemainExclusionCapable) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)
    declare i32 @binary_sem_post(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @binary_sem_wait(i8* %sem)
      call i32 @binary_sem_post(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *binary_wait = &*it++;
  const Instruction *binary_post = &*it++;

  EXPECT_TRUE(api->isSemaphoreOp(binary_wait));
  EXPECT_TRUE(api->isSemaphoreOp(binary_post));
  EXPECT_TRUE(api->isBinarySemaphoreOp(binary_wait));
  EXPECT_TRUE(api->isBinarySemaphoreOp(binary_post));
  EXPECT_TRUE(api->isTDAcquire(binary_wait));
  EXPECT_TRUE(api->isTDRelease(binary_post));
}

TEST_F(ThreadAPITest, RecognizesAdditionalMPICommunicatorManagementAPIs) {
  const char *source = R"(
    declare i32 @MPI_Intercomm_create(i8*, i32, i8*, i32, i32, i8**)
    declare i32 @MPI_Intercomm_merge(i8*, i32, i8**)
    declare i32 @MPI_Comm_disconnect(i8**)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("MPI_Intercomm_create")),
            ThreadAPI::TD_MPI_COMM_CREATE);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Intercomm_merge")),
            ThreadAPI::TD_MPI_COMM_CREATE);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Comm_disconnect")),
            ThreadAPI::TD_MPI_COMM_FREE);
}

TEST_F(ThreadAPITest, RecognizesPersistentMPIRequestLifecycleHelpers) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)

    define i32 @main(i8* %comm, i8* %req1, i8* %req2) {
    entry:
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Recv_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req2)
      call i32 @MPI_Start(i8* %req1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("MPI_Send_init")),
            ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Recv_init")),
            ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT);
  EXPECT_EQ(api->getType(module->getFunction("MPI_Start")),
            ThreadAPI::TD_MPI_REQUEST_START);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *send_init = &*it++;
  const Instruction *recv_init = &*it++;
  const Instruction *start = &*it++;

  EXPECT_TRUE(api->isMPIRequestManagement(send_init));
  EXPECT_TRUE(api->isMPIRequestManagement(recv_init));
  EXPECT_TRUE(api->isMPIRequestManagement(start));
  EXPECT_TRUE(api->isPersistentMPIRequestInit(send_init));
  EXPECT_TRUE(api->isPersistentMPIRequestInit(recv_init));
  EXPECT_TRUE(api->isPersistentMPIRequestStart(start));
}

TEST_F(ThreadAPITest, RecognizesJthreadAndTreatsItAsForkLike) {
  const char *source = R"(
    declare void @_ZNSt7jthreadC1EPFvPvES0_(i8*, i8* (i8*)*, i8*)
    declare void @_ZNSt7jthread4joinEv(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @main() {
    entry:
      %thr = alloca i8
      call void @_ZNSt7jthreadC1EPFvPvES0_(i8* %thr, i8* (i8*)* @worker, i8* null)
      call void @_ZNSt7jthread4joinEv(i8* %thr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *fork = nullptr;
  const Instruction *join = nullptr;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      if (!fork) {
        fork = &inst;
      } else if (!join) {
        join = &inst;
        break;
      }
    }
  }
  ASSERT_NE(fork, nullptr);
  ASSERT_NE(join, nullptr);

  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthreadC1EPFvPvES0_")),
            ThreadAPI::TD_JTHREAD_FORK);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt7jthread4joinEv")),
            ThreadAPI::TD_JTHREAD_JOIN);
  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_TRUE(api->isTDJoin(join));
}

TEST_F(ThreadAPITest, StdThreadMoveConstructorIsNotFork) {
  const char *source = R"(
    declare void @_ZNSt6threadC1EOS_(i8*, i8*)

    define void @main() {
    entry:
      %dst = alloca i8
      %src = alloca i8
      call void @_ZNSt6threadC1EOS_(i8* %dst, i8* %src)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *move_ctor = module->getFunction("_ZNSt6threadC1EOS_");
  ASSERT_NE(move_ctor, nullptr);
  EXPECT_EQ(api->getType(move_ctor), ThreadAPI::TD_DUMMY);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  EXPECT_FALSE(api->isTDFork(call));
}

TEST_F(ThreadAPITest, RecognizesLibcxxJoinDetachManglings) {
  const char *source = R"(
    declare void @_ZNSt3__16thread4joinEv(i8*)
    declare void @_ZNSt3__16thread6detachEv(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt3__16thread4joinEv")),
            ThreadAPI::TD_JOIN);
  EXPECT_EQ(api->getType(module->getFunction("_ZNSt3__16thread6detachEv")),
            ThreadAPI::TD_DETACH);
}

TEST_F(ThreadAPITest, UnwrapsConditionVariableAnyWaitMutexFromUniqueLock) {
  const char *source = R"(
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @_ZNSt22condition_variable_any4waitERSt11unique_lockISt5mutexE(i8*, i8*)

    @cv = global i8 0
    @lock = global i8 0

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @_ZNSt22condition_variable_any4waitERSt11unique_lockISt5mutexE(i8* @cv, i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Function *wait_func = module->getFunction(
      "_ZNSt22condition_variable_any4waitERSt11unique_lockISt5mutexE");
  ASSERT_NE(wait_func, nullptr);
  EXPECT_EQ(api->getType(wait_func), ThreadAPI::TD_COND_WAIT);
  auto it = main_func->getEntryBlock().begin();
  ++it;
  ++it;
  const Instruction *wait = &*it;
  ASSERT_TRUE(api->isTDCondWait(wait));
  EXPECT_EQ(api->getCondVal(wait), module->getNamedGlobal("cv"));
  EXPECT_EQ(api->getCondMutex(wait), module->getNamedGlobal("lock"));
}

TEST_F(ThreadAPITest, StdJthreadMoveConstructorIsNotFork) {
  const char *source = R"(
    declare void @_ZNSt7jthreadC1EOS_(i8*, i8*)

    define void @main() {
    entry:
      %dst = alloca i8
      %src = alloca i8
      call void @_ZNSt7jthreadC1EOS_(i8* %dst, i8* %src)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *move_ctor = module->getFunction("_ZNSt7jthreadC1EOS_");
  ASSERT_NE(move_ctor, nullptr);
  EXPECT_EQ(api->getType(move_ctor), ThreadAPI::TD_DUMMY);
}

TEST_F(ThreadAPITest, RecognizesGNUOpenMPParallelForkAndBarrierVariants) {
  const char *source = R"(
    declare void @GOMP_parallel(void ()*, i8*, i32, i32)
    declare void @GOMP_parallel_end()

    define void @worker() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @GOMP_parallel(void ()* @worker, i8* null, i32 1, i32 0)
      call void @GOMP_parallel_end()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_parallel")),
            ThreadAPI::TD_FORK);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_parallel_end")),
            ThreadAPI::TD_BAR_WAIT);

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  auto it = main_func->getEntryBlock().begin();
  const Instruction *fork = &*it++;
  const Instruction *end = &*it++;

  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_EQ(api->getForkedThread(fork), nullptr);
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction("worker"));
  EXPECT_TRUE(api->isTDBarWait(end));
}

TEST_F(ThreadAPITest, RecognizesGNUOpenMPTaskloopPrefixVariants) {
  const char *source = R"(
    declare void @GOMP_taskloop(i8*)
    declare void @GOMP_taskloop_ull(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskloop")),
            ThreadAPI::TD_OMP_TASKLOOP);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskloop_ull")),
            ThreadAPI::TD_OMP_TASKLOOP);
}

TEST_F(ThreadAPITest, MapsOpenMPTaskwaitWithDepsVariants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_taskwait(i8*, i32)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait_deps_51(i8*, i32, i32, i8*, i32, i8*, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_taskwait")),
            ThreadAPI::TD_OMP_TASKWAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_wait_deps")),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_taskwait_deps_51")),
            ThreadAPI::TD_OMP_TASKWAIT_DEPS);
}

TEST_F(ThreadAPITest, ClassifiesSharedTimedMutexReleases) {
  const char *source = R"(
    declare void @_ZNSt18shared_timed_mutex13unlock_sharedEv(i8*)
    declare void @_ZNSt18shared_timed_mutex6unlockEv(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction(
                "_ZNSt18shared_timed_mutex13unlock_sharedEv")),
            ThreadAPI::TD_SHARED_UNLOCK);
  EXPECT_EQ(
      api->getType(module->getFunction("_ZNSt18shared_timed_mutex6unlockEv")),
      ThreadAPI::TD_SHARED_UNLOCK);
}

TEST_F(ThreadAPITest, ExtractsOutlinedOpenMPForkTarget) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*)

    define internal void @.omp_outlined.(i32* %gtid, i32* %btid, ...) {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @__kmpc_fork_call(i8* null, i32 0,
                                  void (i32*, i32*, ...)* @.omp_outlined.)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *fork = &main_func->getEntryBlock().front();
  EXPECT_TRUE(api->isTDFork(fork));
  EXPECT_EQ(api->getForkedFun(fork), module->getFunction(".omp_outlined."));
}

TEST_F(ThreadAPITest, SharedLockPredicatesAreConsistentAcrossOverloads) {
  const char *source = R"(
    declare void @_ZNSt12shared_mutex11lock_sharedEv(i8*)
    declare void @_ZNSt12shared_mutex4lockEv(i8*)

    define void @main(i8* %m) {
    entry:
      call void @_ZNSt12shared_mutex11lock_sharedEv(i8* %m)
      call void @_ZNSt12shared_mutex4lockEv(i8* %m)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const auto *shared_call = llvm::dyn_cast<CallBase>(&*it++);
  const auto *exclusive_call = llvm::dyn_cast<CallBase>(&*it++);
  ASSERT_NE(shared_call, nullptr);
  ASSERT_NE(exclusive_call, nullptr);

  EXPECT_EQ(api->isReadLockAcquire(shared_call),
            api->isReadLockAcquire(&*shared_call));
  EXPECT_EQ(api->isWriteLockAcquire(shared_call),
            api->isWriteLockAcquire(&*shared_call));
  EXPECT_EQ(api->isReadLockAcquire(exclusive_call),
            api->isReadLockAcquire(&*exclusive_call));
  EXPECT_EQ(api->isWriteLockAcquire(exclusive_call),
            api->isWriteLockAcquire(&*exclusive_call));
}

TEST_F(ThreadAPITest, MapsOpenMPTaskRuntimeVariants) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)
    declare i32 @__kmpc_omp_task_with_deps_51(i8*, i32, i8*, i32, i8*, i32, i8*, i8*)
    declare void @__kmpc_omp_task_complete_if0(i8*, i32, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_begin_if0")),
            ThreadAPI::TD_OMP_TASK);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_with_deps_51")),
            ThreadAPI::TD_OMP_TASK_WITH_DEPS);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_omp_task_complete_if0")),
            ThreadAPI::TD_OMP_TASK_COMPLETE);
  EXPECT_EQ(api->getRuntimeLibrary(
                module->getFunction("__kmpc_omp_task_with_deps_51")),
            ThreadAPI::RuntimeLibrary::OpenMP);
  EXPECT_EQ(
      api->getSemanticTag(module->getFunction("__kmpc_omp_task_with_deps_51")),
      "task-with-deps");
}

TEST_F(ThreadAPITest, DistinguishesOpenMPDoacrossRuntimeVariants) {
  const char *source = R"(
    declare void @__kmpc_doacross_wait(i8*, i32, i64*)
    declare void @__kmpc_doacross_submit(i8*, i32, i64*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_wait")),
            ThreadAPI::TD_OMP_DOACROSS_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_submit")),
            ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
}

TEST_F(ThreadAPITest, LongestPrefixRuleWinsForSpecializedOpenMPRuntimeFamilies) {
  const char *source = R"(
    declare void @__kmpc_teams_host(i8*, i32)
    declare void @__kmpc_teams_distribute_nowait_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_static_init_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_dynamic_init_4(i8*, i32, i32*)
    declare void @__kmpc_distribute_guidance_init_4(i8*, i32, i32*)
    declare void @__kmpc_loop_static_4(i8*, i32, i32*)
    declare void @__kmpc_loop_dynamic_4(i8*, i32, i32*)
    declare void @__kmpc_loop_guidance_4(i8*, i32, i32*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  EXPECT_EQ(api->getType(module->getFunction("__kmpc_teams_host")),
            ThreadAPI::TD_OMP_TEAMS_HOST);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_teams_distribute_nowait_4")),
            ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_distribute_static_init_4")),
            ThreadAPI::TD_OMP_DISTRIBUTE_STATIC);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_distribute_dynamic_init_4")),
            ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_distribute_guidance_init_4")),
            ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_static_4")),
            ThreadAPI::TD_OMP_LOOP_STATIC_INIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_dynamic_4")),
            ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_loop_guidance_4")),
            ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT);
}

TEST_F(ThreadAPITest, MapsOpenMPRegionRuntimeVariants) {
  const char *source = R"(
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)
    declare i32 @__kmpc_master(i8*, i32)
    declare void @__kmpc_end_master(i8*, i32)
    declare void @__kmpc_ordered(i8*, i32)
    declare void @__kmpc_end_ordered(i8*, i32)
    declare i32 @__kmpc_reduce(i8*, i32, i32, i64, i8*, void (i8*, i8*)*, [8 x i32]*)
    declare void @__kmpc_for_static_fini(i8*, i32)
    declare void @__kmpc_dispatch_fini_4(i8*, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_single")),
            ThreadAPI::TD_OMP_SINGLE_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_single")),
            ThreadAPI::TD_OMP_SINGLE_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_master")),
            ThreadAPI::TD_OMP_MASTER_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_master")),
            ThreadAPI::TD_OMP_MASTER_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_ordered")),
            ThreadAPI::TD_OMP_ORDERED_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_end_ordered")),
            ThreadAPI::TD_OMP_ORDERED_END);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_reduce")),
            ThreadAPI::TD_OMP_REDUCE_START);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_for_static_fini")),
            ThreadAPI::TD_OMP_FOR_STATIC_FINI);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_dispatch_fini_4")),
            ThreadAPI::TD_OMP_FOR_DISPATCH_FINI);
}

TEST_F(ThreadAPITest, DescribesMPIBarrierUsingStructuredConfig) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  ThreadAPI::APIDescription desc =
      api->describe(module->getFunction("MPI_Ibarrier"));
  EXPECT_EQ(desc.type, ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(desc.library, ThreadAPI::RuntimeLibrary::MPI);
  EXPECT_EQ(desc.semantic_tag, "ibarrier");
  EXPECT_TRUE(desc.from_config);
}

TEST_F(ThreadAPITest, UsesCriticalNameAsAnalysisLockIdentity) {
  const char *source = R"(
    @crit = global [8 x i32] zeroinitializer

    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define void @main() {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  const Instruction *enter = &*it++;
  const Instruction *exit = &*it++;

  EXPECT_EQ(api->getAnalysisLockIdentity(enter),
            module->getNamedGlobal("crit"));
  EXPECT_EQ(api->getAnalysisLockIdentity(exit), module->getNamedGlobal("crit"));
}

TEST_F(ThreadAPITest, WrapperOperationsShareAnalysisLockIdentity) {
  const char *source = R"(
    @lock = global i8 0

    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)
    declare void @fake_unique_locklockEv(i8*)
    declare void @fake_unique_lockunlockEv(i8*)

    define void @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @fake_unique_lockunlockEv(i8* %wrapper)
      call void @fake_unique_locklockEv(i8* %wrapper)
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
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
  const Instruction *unlock = calls[1];
  const Instruction *lock = calls[2];
  const Instruction *dtor = calls[3];

  const Value *identity = api->getAnalysisLockIdentity(ctor);
  ASSERT_NE(identity, nullptr);
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(unlock));
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(lock));
  EXPECT_EQ(identity, api->getAnalysisLockIdentity(dtor));
}

TEST_F(ThreadAPITest, ReportsExplicitSemanticLoweringStatus) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  auto async = api->getSemanticLoweringInfo(ThreadAPI::TD_ASYNC);
  auto future_get = api->getSemanticLoweringInfo(ThreadAPI::TD_FUTURE_GET);
  auto omp_atomic = api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_ATOMIC_START);
  auto task_complete =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_TASK_COMPLETE);
  auto doacross_submit =
      api->getSemanticLoweringInfo(ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
  auto atomic_wait = api->getSemanticLoweringInfo(ThreadAPI::TD_ATOMIC_WAIT);

  EXPECT_EQ(async.kind, ThreadAPI::SemanticLoweringKind::Deferred);
  EXPECT_STREQ(async.reason, "async-launch-policy-witness");
  EXPECT_NE(async.owners, 0u);
  EXPECT_EQ(future_get.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(future_get.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_FUTURE_GET, ThreadAPI::SemanticLoweringOwner::HB));
  EXPECT_EQ(omp_atomic.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(omp_atomic.reason, "openmp-atomic-runtime-unmodeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_ATOMIC_START,
      ThreadAPI::SemanticLoweringOwner::ExplicitFallback));
  EXPECT_EQ(task_complete.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(task_complete.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_TASK_COMPLETE, ThreadAPI::SemanticLoweringOwner::OpenMP));
  EXPECT_EQ(doacross_submit.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_STREQ(doacross_submit.reason, "modeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      ThreadAPI::TD_OMP_DOACROSS_SUBMIT, ThreadAPI::SemanticLoweringOwner::HB));
  EXPECT_EQ(atomic_wait.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(atomic_wait.reason, "cpp-atomic-wait-runtime-unmodeled");
}

TEST_F(ThreadAPITest, OpenMPBarrierUsesSiteIdentityInsteadOfMetadataOperand) {
  const char *source = R"(
    declare void @__kmpc_barrier(i8*, i32)

    define void @main() {
    entry:
      call void @__kmpc_barrier(i8* null, i32 0)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *barrier = &main_func->getEntryBlock().front();

  EXPECT_EQ(api->getType(module->getFunction("__kmpc_barrier")),
            ThreadAPI::TD_BAR_WAIT);
  EXPECT_EQ(api->getBarrierVal(barrier), barrier);
}

TEST_F(ThreadAPITest, SpecialSemanticLoweringStatesStayExplicitlyEnumerated) {
  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  std::set<ThreadAPI::TD_TYPE> expected_non_modeled = {
      ThreadAPI::TD_DUMMY,
      ThreadAPI::TD_ASYNC,
      ThreadAPI::TD_OMP_ATOMIC_START,
      ThreadAPI::TD_OMP_ATOMIC_END,
      ThreadAPI::TD_OMP_CANCEL,
      ThreadAPI::TD_OMP_TARGET_DATA_UPDATE,
      ThreadAPI::TD_OMP_TEAMS,
      ThreadAPI::TD_OMP_TEAMS_HOST,
      ThreadAPI::TD_OMP_TEAMS_DISTRIBUTE,
      ThreadAPI::TD_OMP_DISTRIBUTE,
      ThreadAPI::TD_OMP_DISTRIBUTE_STATIC,
      ThreadAPI::TD_OMP_DISTRIBUTE_DYNAMIC,
      ThreadAPI::TD_OMP_DISTRIBUTE_GUIDANCE,
      ThreadAPI::TD_OMP_LOOP_STATIC_INIT,
      ThreadAPI::TD_OMP_LOOP_DYNAMIC_INIT,
      ThreadAPI::TD_OMP_LOOP_GUIDANCE_INIT,
      ThreadAPI::TD_OMP_AFFINITY,
      ThreadAPI::TD_OMP_SCOPE_START,
      ThreadAPI::TD_OMP_SCOPE_END,
      ThreadAPI::TD_OMP_TASKLOOP_SIMD,
      ThreadAPI::TD_OMP_TASKLOOP_FINI,
      ThreadAPI::TD_OMP_INTEROP_INIT,
      ThreadAPI::TD_OMP_INTEROP_FINI,
      ThreadAPI::TD_SEMAPHORE_ACQUIRE,
      ThreadAPI::TD_SEMAPHORE_RELEASE,
      ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE,
      ThreadAPI::TD_ATOMIC_WAIT,
      ThreadAPI::TD_ATOMIC_NOTIFY_ONE,
      ThreadAPI::TD_ATOMIC_NOTIFY_ALL,
      ThreadAPI::TD_MPI_SESSION_GET_INFO,
      ThreadAPI::TD_MPI_SESSION_GET_NUM_ERRCODES,
      ThreadAPI::TD_MPI_SESSION_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_SESSION_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_ERRHANDLER_CREATE,
      ThreadAPI::TD_MPI_ERRHANDLER_FREE,
      ThreadAPI::TD_MPI_COMM_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_COMM_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_COMM_CALL_ERRHANDLER,
      ThreadAPI::TD_MPI_WIN_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_WIN_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_FILE_GET_ERRHANDLER,
      ThreadAPI::TD_MPI_FILE_SET_ERRHANDLER,
      ThreadAPI::TD_MPI_ERROR_CLASS,
      ThreadAPI::TD_MPI_ERROR_STRING,
      ThreadAPI::TD_MPI_INFO_CREATE,
      ThreadAPI::TD_MPI_INFO_DUP,
      ThreadAPI::TD_MPI_INFO_FREE,
      ThreadAPI::TD_MPI_INFO_GET,
      ThreadAPI::TD_MPI_INFO_GET_VALUELEN,
      ThreadAPI::TD_MPI_INFO_GET_NKEYS,
      ThreadAPI::TD_MPI_INFO_GET_NTHKEY,
      ThreadAPI::TD_MPI_INFO_GET_KEYVAL,
      ThreadAPI::TD_MPI_INFO_SET,
      ThreadAPI::TD_MPI_INFO_DELETE,
      ThreadAPI::TD_MPI_INFO_C2F,
      ThreadAPI::TD_MPI_INFO_CREATE_ENV,
      ThreadAPI::TD_MPI_INFO_FREE_ENV,
      ThreadAPI::TD_MPI_GET_COUNT,
      ThreadAPI::TD_MPI_GET_ELEMENTS,
      ThreadAPI::TD_MPI_GET_ELEMENTS_X,
      ThreadAPI::TD_MPI_STATUS_SIZE,
      ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS,
      ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS_X,
  };

  for (int raw = static_cast<int>(ThreadAPI::TD_DUMMY);
       raw <= static_cast<int>(ThreadAPI::TD_KERNEL_MEMORY_BARRIER); ++raw) {
    ThreadAPI::TD_TYPE type = static_cast<ThreadAPI::TD_TYPE>(raw);
    const char *name = ThreadAPI::tdTypeToString(type);
    ASSERT_NE(name, nullptr);
    ASSERT_NE(name[0], '\0');

    ThreadAPI::SemanticLoweringInfo info = api->getSemanticLoweringInfo(type);
    ASSERT_NE(info.reason, nullptr);
    EXPECT_NE(info.reason[0], '\0') << name;

    if (expected_non_modeled.count(type) != 0) {
      EXPECT_NE(info.kind, ThreadAPI::SemanticLoweringKind::Modeled) << name;
      if (type != ThreadAPI::TD_ASYNC &&
          info.kind != ThreadAPI::SemanticLoweringKind::Deferred) {
        EXPECT_NE(info.owners & ThreadAPI::semanticLoweringOwnerMask(
                                  ThreadAPI::SemanticLoweringOwner::ExplicitFallback),
                  0u)
            << name;
      }
    } else {
      EXPECT_EQ(info.kind, ThreadAPI::SemanticLoweringKind::Modeled) << name;
      EXPECT_NE(info.owners, 0u) << name;
    }
  }
}

TEST_F(ThreadAPITest, ModeledConcurrencyFunctionsExposeConcreteLoweringOwners) {
  const char *source = R"(
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)

    @crit = global [8 x i32] zeroinitializer

    define void @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *critical = module->getFunction("__kmpc_critical");
  const Function *idup = module->getFunction("MPI_Comm_idup");
  ASSERT_NE(critical, nullptr);
  ASSERT_NE(idup, nullptr);

  ThreadAPI::SemanticLoweringInfo critical_info =
      api->getSemanticLoweringInfo(critical);
  EXPECT_EQ(critical_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      critical, ThreadAPI::SemanticLoweringOwner::OpenMP));
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      critical, ThreadAPI::SemanticLoweringOwner::LockSet));

  ThreadAPI::SemanticLoweringInfo idup_info = api->getSemanticLoweringInfo(idup);
  EXPECT_EQ(idup_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      idup, ThreadAPI::SemanticLoweringOwner::MPI));
}

TEST_F(ThreadAPITest, LongestPrefixRuleWinsForOpenMPDoacross) {
  const char *source = R"(
    declare void @__kmpc_doacross_wait_4(i8*, i32, i64*)
    declare void @__kmpc_doacross_submit_4(i8*, i32, i64*)
    declare void @__kmpc_doacross_init_4(i8*, i32, i64*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_wait_4")),
            ThreadAPI::TD_OMP_DOACROSS_WAIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_submit_4")),
            ThreadAPI::TD_OMP_DOACROSS_SUBMIT);
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_doacross_init_4")),
            ThreadAPI::TD_OMP_DOACROSS_INIT);
}

TEST_F(ThreadAPITest, SemaphoreLoweringIsExplicitForBinaryAndCountingForms) {
  const char *source = R"(
    declare i32 @binary_sem_wait(i8*)
    declare void @_ZNSt16binary_semaphore7acquireEv(i8*)
    declare void @_ZNSt18counting_semaphore7acquireEv(i8*)

    define void @main(i8* %sem) {
    entry:
      call i32 @binary_sem_wait(i8* %sem)
      call void @_ZNSt16binary_semaphore7acquireEv(i8* %sem)
      call void @_ZNSt18counting_semaphore7acquireEv(i8* %sem)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *binary_sem_wait = module->getFunction("binary_sem_wait");
  const Function *binary_cpp =
      module->getFunction("_ZNSt16binary_semaphore7acquireEv");
  const Function *counting_cpp =
      module->getFunction("_ZNSt18counting_semaphore7acquireEv");
  ASSERT_NE(binary_sem_wait, nullptr);
  ASSERT_NE(binary_cpp, nullptr);
  ASSERT_NE(counting_cpp, nullptr);

  auto binary_info = api->getSemanticLoweringInfo(binary_sem_wait);
  EXPECT_EQ(binary_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      binary_sem_wait, ThreadAPI::SemanticLoweringOwner::LockSet));

  auto binary_cpp_info = api->getSemanticLoweringInfo(binary_cpp);
  EXPECT_EQ(binary_cpp_info.kind, ThreadAPI::SemanticLoweringKind::Modeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      binary_cpp, ThreadAPI::SemanticLoweringOwner::LockSet));

  auto counting_info = api->getSemanticLoweringInfo(counting_cpp);
  EXPECT_EQ(counting_info.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_STREQ(counting_info.reason, "counting-semaphore-runtime-unmodeled");
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      counting_cpp, ThreadAPI::SemanticLoweringOwner::ExplicitFallback));

  auto generic_info =
      api->getSemanticLoweringInfo(ThreadAPI::TD_SEMAPHORE_ACQUIRE);
  EXPECT_EQ(generic_info.kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
}

TEST_F(ThreadAPITest, RecognizesCppAtomicWaitNotifyAndJthreadDestructor) {
  const char *source = R"(
    declare void @_ZNSt6atomicIiE4waitEi(i8*, i32)
    declare void @_ZNSt6atomicIiE10notify_oneEv(i8*)
    declare void @_ZNSt6atomicIiE10notify_allEv(i8*)
    declare void @_ZNSt7jthreadD1Ev(i8*)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  const Function *atomic_wait = module->getFunction("_ZNSt6atomicIiE4waitEi");
  const Function *notify_one =
      module->getFunction("_ZNSt6atomicIiE10notify_oneEv");
  const Function *notify_all =
      module->getFunction("_ZNSt6atomicIiE10notify_allEv");
  const Function *jthread_dtor = module->getFunction("_ZNSt7jthreadD1Ev");
  ASSERT_NE(atomic_wait, nullptr);
  ASSERT_NE(notify_one, nullptr);
  ASSERT_NE(notify_all, nullptr);
  ASSERT_NE(jthread_dtor, nullptr);

  EXPECT_EQ(api->getType(atomic_wait), ThreadAPI::TD_ATOMIC_WAIT);
  EXPECT_EQ(api->getType(notify_one), ThreadAPI::TD_ATOMIC_NOTIFY_ONE);
  EXPECT_EQ(api->getType(notify_all), ThreadAPI::TD_ATOMIC_NOTIFY_ALL);
  EXPECT_EQ(api->getType(jthread_dtor), ThreadAPI::TD_JTHREAD_DTOR);
  EXPECT_EQ(api->getSemanticLoweringInfo(atomic_wait).kind,
            ThreadAPI::SemanticLoweringKind::RecognizedButUnmodeled);
  EXPECT_TRUE(api->hasSemanticLoweringOwner(
      jthread_dtor, ThreadAPI::SemanticLoweringOwner::MHP));
}

TEST_F(ThreadAPITest, MPIConfiguredAPIsHaveConsistentLoweringLibraries) {
  const char *source = R"(
    declare i32 @MPI_Session_get_info(i8*, i8*)
    declare i32 @MPI_Type_get_extent(i32, i64*, i64*)
    declare i32 @MPI_Cart_create(i8*, i32, i32*, i32*, i32, i8*)

    define i32 @main(i8* %session, i8* %info, i64* %lb, i64* %extent,
                     i8* %comm, i32* %dims, i32* %periods, i8* %newcomm) {
    entry:
      call i32 @MPI_Session_get_info(i8* %session, i8* %info)
      call i32 @MPI_Type_get_extent(i32 0, i64* %lb, i64* %extent)
      call i32 @MPI_Cart_create(i8* %comm, i32 1, i32* %dims, i32* %periods,
                                i32 0, i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();

  const Function *session = module->getFunction("MPI_Session_get_info");
  const Function *extent = module->getFunction("MPI_Type_get_extent");
  const Function *cart = module->getFunction("MPI_Cart_create");
  ASSERT_NE(session, nullptr);
  ASSERT_NE(extent, nullptr);
  ASSERT_NE(cart, nullptr);

  EXPECT_NE(api->getType(session), ThreadAPI::TD_DUMMY);
  EXPECT_NE(api->getType(extent), ThreadAPI::TD_DUMMY);
  EXPECT_NE(api->getType(cart), ThreadAPI::TD_DUMMY);

  auto session_type_info =
      api->getSemanticLoweringInfo(api->getType(session));
  auto session_func_info = api->getSemanticLoweringInfo(session);
  EXPECT_EQ(session_type_info.kind, session_func_info.kind);
  EXPECT_EQ(session_type_info.owners, session_func_info.owners);

  auto extent_type_info =
      api->getSemanticLoweringInfo(api->getType(extent));
  auto extent_func_info = api->getSemanticLoweringInfo(extent);
  EXPECT_EQ(extent_type_info.kind, extent_func_info.kind);
  EXPECT_EQ(extent_type_info.owners, extent_func_info.owners);

  auto cart_type_info = api->getSemanticLoweringInfo(api->getType(cart));
  auto cart_func_info = api->getSemanticLoweringInfo(cart);
  EXPECT_EQ(cart_type_info.kind, cart_func_info.kind);
  EXPECT_EQ(cart_type_info.owners, cart_func_info.owners);
}

TEST_F(ThreadAPITest, NormalizesWrappedAndOpenMPIForwarderNames) {
  const char *source = R"(
    declare i32 @__wrap_MPI_Barrier(i8*)
    declare i32 @__wrap_PMPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @ompi_mpi_allreduce(i8*, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @__wrap_MPI_Barrier(i8* %comm)
      call i32 @__wrap_PMPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @ompi_mpi_allreduce(i8* null, i8* null, i32 1, i32 0, i32 0,
                                   i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__wrap_MPI_Barrier")),
            ThreadAPI::TD_MPI_BARRIER);
  EXPECT_EQ(api->getType(module->getFunction("__wrap_PMPI_Bcast")),
            ThreadAPI::TD_MPI_BCAST);
  EXPECT_EQ(api->getType(module->getFunction("ompi_mpi_allreduce")),
            ThreadAPI::TD_MPI_ALLREDUCE);
  EXPECT_EQ(api->getRuntimeLibrary(module->getFunction("ompi_mpi_allreduce")),
            ThreadAPI::RuntimeLibrary::MPI);
}

TEST_F(ThreadAPITest, RecognizesGOMPTaskAndBarrierRuntimeAliases) {
  const char *source = R"(
    declare void @GOMP_barrier()
    declare void @GOMP_taskwait()
    declare void @GOMP_taskgroup_start()
    declare void @GOMP_taskgroup_end()
    declare void @GOMP_task(void ()*, i8*, i8*, i64, i64, i1, i32, i8*, i32)

    define void @worker() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @GOMP_task(void ()* @worker, i8* null, i8* null, i64 0, i64 0,
                           i1 true, i32 0, i8* null, i32 0)
      call void @GOMP_taskwait()
      call void @GOMP_taskgroup_start()
      call void @GOMP_taskgroup_end()
      call void @GOMP_barrier()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("GOMP_task")),
            ThreadAPI::TD_OMP_TASK);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskwait")),
            ThreadAPI::TD_OMP_TASKWAIT);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskgroup_start")),
            ThreadAPI::TD_OMP_TASKGROUP_START);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_taskgroup_end")),
            ThreadAPI::TD_OMP_TASKGROUP_END);
  EXPECT_EQ(api->getType(module->getFunction("GOMP_barrier")),
            ThreadAPI::TD_BAR_WAIT);
}

TEST_F(ThreadAPITest, RecognizesCriticalWithHintAsCriticalEntry) {
  const char *source = R"(
    declare void @__kmpc_critical_with_hint(i8*, i32, i8*, i64)

    define void @main(i8* %lock) {
    entry:
      call void @__kmpc_critical_with_hint(i8* null, i32 0, i8* %lock, i64 1)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ThreadAPI::resetThreadAPI();
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  EXPECT_EQ(api->getType(module->getFunction("__kmpc_critical_with_hint")),
            ThreadAPI::TD_ACQUIRE);
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  EXPECT_EQ(api->getAnalysisLockIdentity(call),
            cast<CallBase>(call)->getArgOperand(2));
}

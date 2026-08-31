add_subdirectory(MHP)
add_subdirectory(LockSet)
add_subdirectory(Memory)
add_subdirectory(ValueFlow)
add_subdirectory(JoinTarget)
add_subdirectory(Utils/Clocks)
add_subdirectory(Utils/ThreadAPI)
add_subdirectory(Utils/ThreadLocal)
add_subdirectory(OpenMP)
add_subdirectory(MPI)
add_subdirectory(CUDA)
add_subdirectory(LinuxKernel)

# Preserve the established build entry point while component suites remain
# independently buildable and runnable through CTest.
add_custom_target(concurrency_core_tests
  DEPENDS
    concurrency_mhp_tests
    concurrency_happens_before_tests
    concurrency_atomic_tests
    concurrency_lockset_tests
    concurrency_memory_tests
    concurrency_valueflow_tests
    concurrency_join_target_tests
    concurrency_clock_tests
    concurrency_thread_api_tests
    concurrency_thread_local_tests
    concurrency_cuda_tests
    concurrency_linux_kernel_tests)

add_lotus_test_suite(utils_parallel_tests
  SOURCES
    PipelineSchedulerTest.cpp
    PipelineSchedulerTimeoutTest.cpp
    ParallelSchedulerPassTest.cpp
    ThreadSafeTest.cpp
  LINK_LIBS
    CanaryParallel)

add_executable(thread_pool_harness_test ThreadPoolHarnessTest.cpp)
set_target_properties(thread_pool_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(thread_pool_harness_test PRIVATE ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(thread_pool_harness_test lotus_test_harness_utils CanaryParallel)

add_test(NAME thread_pool_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/thread_pool_harness_test -nworkers=0)
set_tests_properties(thread_pool_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;utils;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME thread_pool_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/thread_pool_harness_test -nworkers=2)
set_tests_properties(thread_pool_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;utils;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(parallel_tabulation_harness_test ParallelTabulationHarnessTest.cpp)
set_target_properties(parallel_tabulation_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(parallel_tabulation_harness_test PRIVATE ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(parallel_tabulation_harness_test
  lotus_test_harness_utils
  CanaryParallel
  CanaryCSIndex)

add_test(NAME parallel_tabulation_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/parallel_tabulation_harness_test -nworkers=0)
set_tests_properties(parallel_tabulation_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;utils;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME parallel_tabulation_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/parallel_tabulation_harness_test -nworkers=2)
set_tests_properties(parallel_tabulation_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;utils;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(pipeline_scheduler_harness_test PipelineSchedulerHarnessTest.cpp)
set_target_properties(pipeline_scheduler_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(pipeline_scheduler_harness_test PRIVATE ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(pipeline_scheduler_harness_test lotus_test_harness_utils CanaryParallel)

add_test(NAME pipeline_scheduler_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/pipeline_scheduler_harness_test -nworkers=2)
set_tests_properties(pipeline_scheduler_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;utils;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

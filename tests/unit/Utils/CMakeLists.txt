# Utility Unit Tests

add_lotus_utils_test(graph_slicer_test GraphSlicerTest.cpp)

# Other Tests
add_lotus_utils_test(egraph_test EGraphTest.cpp)
add_lotus_utils_test(string_utils_test StringUtilsTest.cpp)
add_lotus_utils_test(path_expressions_test PathExpressionsTest.cpp)
add_lotus_utils_test(pipeline_scheduler_test PipelineSchedulerTest.cpp)
add_lotus_utils_test(pipeline_scheduler_timeout_test PipelineSchedulerTimeoutTest.cpp)
add_lotus_utils_test(thread_safe_test ThreadSafeTest.cpp)

add_executable(thread_pool_harness_test ThreadPoolHarnessTest.cpp)
set_target_properties(thread_pool_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(thread_pool_harness_test PRIVATE ${COMMON_INCLUDES})
target_link_libraries(thread_pool_harness_test
  lotus_test_harness_utils
  CanaryParallel)

add_test(NAME thread_pool_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/thread_pool_harness_test -nworkers=0)
set_tests_properties(thread_pool_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME thread_pool_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/thread_pool_harness_test -nworkers=2)
set_tests_properties(thread_pool_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(parallel_tabulation_harness_test ParallelTabulationHarnessTest.cpp)
set_target_properties(parallel_tabulation_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(parallel_tabulation_harness_test PRIVATE ${COMMON_INCLUDES})
target_link_libraries(parallel_tabulation_harness_test
  lotus_test_harness_utils
  CanaryParallel
  CanaryCSIndex)

add_test(NAME parallel_tabulation_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/parallel_tabulation_harness_test -nworkers=0)
set_tests_properties(parallel_tabulation_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_test(NAME parallel_tabulation_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/parallel_tabulation_harness_test -nworkers=2)
set_tests_properties(parallel_tabulation_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(pipeline_scheduler_harness_test PipelineSchedulerHarnessTest.cpp)
set_target_properties(pipeline_scheduler_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(pipeline_scheduler_harness_test PRIVATE ${COMMON_INCLUDES})
target_link_libraries(pipeline_scheduler_harness_test
  lotus_test_harness_utils
  CanaryParallel)

add_test(NAME pipeline_scheduler_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/pipeline_scheduler_harness_test -nworkers=2)
set_tests_properties(pipeline_scheduler_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

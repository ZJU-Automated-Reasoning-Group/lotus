# VASCO DataFlow Unit Tests

add_lotus_test_suite(vasco_tests
  SOURCES
    VASCOTest.cpp
    VASCOLLVMClientTest.cpp
    VASCOAdditionalLLVMClientTest.cpp
  LINK_LIBS VASCODataFlow)

add_executable(vasco_parallel_harness_test VASCOParallelHarnessTest.cpp)
set_target_properties(vasco_parallel_harness_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_include_directories(vasco_parallel_harness_test PRIVATE
  ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(vasco_parallel_harness_test
  lotus_test_harness_utils
  VASCODataFlow)

add_test(NAME vasco_parallel_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/vasco_parallel_harness_test -nworkers=0)
set_tests_properties(vasco_parallel_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;dataflow;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME vasco_parallel_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/vasco_parallel_harness_test -nworkers=2)
set_tests_properties(vasco_parallel_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;dataflow;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# NPA DataFlow Unit Tests

add_lotus_test_suite(npa_tests
  SOURCES
    NPASolverTest.cpp
    TensorProductLiftTest.cpp
    NPATensorRegularizationTest.cpp
    Interprocedural/Client/NPAInterproceduralClientTest.cpp
    PredicateRelationDomainTest.cpp
    Interprocedural/Taint/NPAInterproceduralTaintTest.cpp
    Interprocedural/NPAInterproceduralNullabilityTest.cpp
    NPAThreadSafetyHardeningTest.cpp
  LINK_LIBS
    NPADataFlow
    CanaryAliasAnalysisWrapper
    CanaryParallel)

add_lotus_test_suite(npa_parallel_rhs_harness_test
  TEST_KIND COMPONENT
  SOURCES NPAParallelRhsHarnessTest.cpp
  LINK_LIBS
    NPADataFlow
    CanaryAliasAnalysisWrapper
    CanaryParallel)
add_test(NAME npa_parallel_rhs_harness_serial_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/npa_parallel_rhs_harness_test -nworkers=0)
set_tests_properties(npa_parallel_rhs_harness_serial_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;dataflow;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_test(NAME npa_parallel_rhs_harness_parallel_test
  COMMAND ${LOTUS_TEST_BIN_DIR}/npa_parallel_rhs_harness_test -nworkers=2)
set_tests_properties(npa_parallel_rhs_harness_parallel_test PROPERTIES
  TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
  LABELS "lotus;component;dataflow;parallel"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

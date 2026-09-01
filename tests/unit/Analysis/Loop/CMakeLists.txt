# Analysis Loop Unit Tests

add_lotus_test_suite(loop_analysis_tests
  TEST_KIND INTEGRATION
  SOURCES
    LoopAnalysisFoundationTest.cpp
    LoopDependenceGraphTest.cpp
    LoopLDGBuilderTest.cpp
    LoopScalarAnalysisTest.cpp
    LoopEnvironmentAndCarriedDepsTest.cpp
    LoopIterationSpaceTest.cpp
    LoopClassificationTest.cpp
    LoopNestingGraphTest.cpp
    LoopParityIVTest.cpp
    LoopParityIterationSpaceTest.cpp
    LoopParitySCCDAGAttrsTest.cpp
  LINK_LIBS
    CanaryLoopAnalysis
    CanaryCFG
    CanaryDebugInfo
    CanaryPDG
    CanaryAliasAnalysisWrapper
    LLVMAnalysis
  COMPILE_DEFINITIONS
    LOTUS_NOELLE_LOOP_PARITY_FIXTURE_DIR="${NOELLE_LOOP_PARITY_SRC_DIR}"
    LOTUS_NOELLE_LOOP_PARITY_LL_DIR="${NOELLE_LOOP_PARITY_LL_DIR}"
  DEPENDS noelle_loop_parity_ll_fixtures)

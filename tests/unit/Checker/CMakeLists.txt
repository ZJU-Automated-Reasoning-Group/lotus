add_subdirectory(AE)
add_subdirectory(Concurrency)
add_subdirectory(Framework)
add_subdirectory(KINT)
add_subdirectory(Pulse)
add_subdirectory(Saber)

lotus_collect_test_sources(checker_tests ReportFormattingTest.cpp)

add_lotus_collected_test_suite(checker_tests
  LINK_LIBS
    AE
    CanaryAliasAnalysisWrapper
    PulseChecker
    ConcurrencyChecker
    CanaryKint
    Saber
    CheckerFramework
    CanaryDebugInfo
    CanaryFormats
    CanaryLLVMUtils
    ${Z3_LIBRARIES}
  COMPILE_DEFINITIONS USE_Z3)

add_lotus_test_suite(fuzz_target_generation_test
  SOURCES FuzzTargetGenerationTest.cpp
  LINK_LIBS
    CanaryFuzzing
    CheckerFramework)

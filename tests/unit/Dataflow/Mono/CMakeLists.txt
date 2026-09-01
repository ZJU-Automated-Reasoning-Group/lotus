# Monotone DataFlow Unit Tests

add_lotus_test_suite(mono_test
  SOURCES
    MonoTest.cpp
    MonoDomainTest.cpp
    MonoSolverContextSensitivityTest.cpp
    MonoSolverContextEdgeCasesTest.cpp
  LINK_LIBS
    MONODataFlow
    CanaryAliasAnalysisWrapper)

# Monotone DataFlow Unit Tests

# Mono Test
add_lotus_mono_test(mono_test MonoTest.cpp)
target_sources(mono_test PRIVATE
  MonoDomainTest.cpp
  MonoSolverContextSensitivityTest.cpp
  MonoSolverContextEdgeCasesTest.cpp)

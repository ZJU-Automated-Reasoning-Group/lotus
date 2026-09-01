add_lotus_concurrency_test_suite(concurrency_mhp_tests
  MHPAnalysisTest.cpp
  MHPInterproceduralAndJoinTest.cpp
  StaticVectorClockMHPTest.cpp)

add_lotus_concurrency_test_suite(concurrency_happens_before_tests
  HappensBeforeAnalysisTest.cpp
  HappensBeforeBarrierAndOpenMPTest.cpp
  HappensBeforeAtomicWitnessTest.cpp
  HappensBeforeAtomicEdgeCasesTest.cpp)

add_lotus_concurrency_test_suite(concurrency_atomic_tests
  AtomicTest.cpp
  AtomicNonConstantTest.cpp
  AtomicFenceAndReleaseSequenceTest.cpp)

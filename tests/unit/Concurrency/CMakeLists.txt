add_lotus_concurrency_test_suite(concurrency_core_tests
  MHPAnalysisTest.cpp
  StaticVectorClockMHPTest.cpp
  HappensBeforeAnalysisTest.cpp
  AtomicTest.cpp
  LockSetAnalysisTest.cpp
  EscapeAnalysisTest.cpp
  StaticThreadSharingAnalysisTest.cpp
  ThreadLocalAnalysisTest.cpp
  ThreadAPITest.cpp
  JoinTargetAnalysisTest.cpp)

add_lotus_concurrency_test_suite(concurrency_openmp_tests
  OpenMPTaskGraphTest.cpp
  OpenMPSemanticsTest.cpp
  DataSharingAnalysisTest.cpp
  ConcurrencyFacadeTest.cpp)

add_lotus_concurrency_test_suite(concurrency_mpi_tests
  MPIAnalysisTest.cpp
  MPIAnalysisTest_Collectives.cpp
  MPIAnalysisTest_RMA.cpp
  MPIAnalysisTest_Communicators.cpp
  MPIAnalysisTest_State.cpp)

add_lotus_concurrency_test_suite(concurrency_mpi_rank_tests
  MPIRankAnalysisTest_Basic.cpp
  MPIRankAnalysisTest_Range.cpp
  MPIRankAnalysisTest_Predicate.cpp)

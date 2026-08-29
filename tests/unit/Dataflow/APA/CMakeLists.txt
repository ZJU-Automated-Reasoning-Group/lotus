# APA DataFlow Unit Tests

add_lotus_test_suite(apa_tests
  SOURCES
    EliminationGraphSolverTest.cpp
    EliminationADTTest.cpp
    EliminationLLVMIntraTest.cpp
    EliminationInterproceduralTest.cpp
    AffineRelationDomainTest.cpp
    StandardDomainTest.cpp
    InterAffineCallAndControlFlowTest.cpp
    InterAffinePredicateTest.cpp
    InterAffineCastAndArithmeticTest.cpp
    InterAffineBitwiseCongruenceTest.cpp
    InterSummaryTransferTest.cpp
    PathSummaryEquationSolverTest.cpp
  LINK_LIBS APADataFlow)

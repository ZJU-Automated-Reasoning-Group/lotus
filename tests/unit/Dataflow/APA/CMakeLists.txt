# APA DataFlow Unit Tests

add_lotus_test_suite(apa_tests
  SOURCES
    EliminationTest.cpp
    AffineRelationDomainTest.cpp
    InterAffineEqualitiesTest.cpp
    InterSummaryTransferTest.cpp
    PathSummaryEquationSolverTest.cpp
    EAN/RoundtripTest.cpp
  LINK_LIBS APADataFlow LotusEGraph)

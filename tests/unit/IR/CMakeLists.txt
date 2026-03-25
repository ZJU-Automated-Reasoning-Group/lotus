add_lotus_ir_test_suite(ir_pdg_tests
  PDGQueryTest.cpp
  PDGSupportUtilsTest.cpp
  CypherQueryTest.cpp
  PropertyBasedSlicingTest.cpp)

add_lotus_svfg_test_suite(ir_svfg_tests
  SVFGSerializerTest.cpp
  SVFGMemorySSATest.cpp)

add_lotus_gsa_test_suite(ir_gsa_tests
  GSATest.cpp)

add_lotus_gvfg_test_suite(ir_gvfg_tests
  GuardedValueFlowTest.cpp
  GuardedValueFlowSerializerTest.cpp
  GuardedValueFlowAdapterShapeTest.cpp
  GuardedValueFlowParityTest.cpp
  GuardedValueFlowSolverTest.cpp)

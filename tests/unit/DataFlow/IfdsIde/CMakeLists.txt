# IFDS/IDE Unit Tests

add_lotus_ifdside_test_suite(ifdside_solver_tests
  IFDSFrameworkCoreTest.cpp
  IFDSSolverTest.cpp
  TaintAnalysisTest.cpp
  IFDSSolverInvariantsTest.cpp
  PathAwareSolverTest.cpp
  ReturnSiteAndConfigTest.cpp)

add_lotus_ifdside_test_suite(ifdside_ide_tests
  IDEEdgeFunctionTest.cpp
  IDEConstantPropagationTest.cpp
  IDEJumpFunctionJoinTest.cpp
  IDENewAnalysesTest.cpp)

add_lotus_ifdside_test_suite(ifdside_llvm_flow_tests
  LLVMFlowHelpersTest.cpp
  LLVMFlowHelpersMappingTest.cpp)

add_lotus_ifdside_test_suite(ifdside_alias_integration_tests
  IFDSClientAliasFlowTest.cpp
  IFDSSolverAliasIntegrationTest.cpp)

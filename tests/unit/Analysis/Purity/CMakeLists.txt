# Analysis Purity Unit Tests

add_lotus_test_suite(purity_analysis_tests
  SOURCES
    FunctionPurityAnalysisTest.cpp
    ExternalPuritySummaryStoreTest.cpp
    PurityInferenceDriverTest.cpp
    PurityUnknownImpactPassTest.cpp
    PurityAttrInferencePassTest.cpp
  LINK_LIBS
    CanaryPurityAnalysis
    CanaryShadowMemSSA
    AliasSpecManager
    LLVMAnalysis)

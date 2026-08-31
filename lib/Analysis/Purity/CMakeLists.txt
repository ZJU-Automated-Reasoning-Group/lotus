add_library(CanaryPurityAnalysis STATIC
  PuritySummary.cpp
  DeclarationSummaryProvider.cpp
  ExternalPuritySummaryStore.cpp
  MemorySSAPuritySummary.cpp
  FunctionPurityAnalysis.cpp
  PurityAttrInferencePass.cpp
  PurityInferenceDriver.cpp
  PurityUnknownImpactPass.cpp
)

target_link_libraries(CanaryPurityAnalysis
  PUBLIC
    AliasSpecManager
    CanaryShadowMemSSA
    #LLVMCore
    LLVMAnalysis
)

# IFDS Framework - Independent Analysis Library
#
# Layout: Debug/, Clients/
# Solver implementations are header-only (include/Dataflow/IFDS/Solvers/*.tpp).

# Framework core
set(IFDS_FRAMEWORK_SOURCES
  Debug/IFDSDebugUtils.cpp
)

# Client analyses
set(IFDS_CLIENT_SOURCES
  Clients/IFDSTaintAnalysis.cpp
  Clients/IFDSTaintAnalysisFlows.cpp
  Clients/IFDSTaintAnalysisReporting.cpp
  Clients/IFDSReachingDefinitions.cpp
  Clients/IDEConstantPropagation.cpp
  Clients/IDETypeState.cpp
  Clients/IDEProtoAnalysis.cpp
  Clients/IDEExtendedTaintAnalysis.cpp
  Clients/IDEFeatureTaintAnalysis.cpp
  Clients/IDESecureHeapPropagation.cpp
  Clients/IDEInstInteractionAnalysis.cpp
  Clients/IDEGeneralizedLCA.cpp
  Clients/IFDSUninitializedVariables.cpp
  Clients/IFDSConstAnalysis.cpp
  Clients/IDELinearConstantAnalysis.cpp
  Clients/IFDSTypeAnalysis.cpp
  Clients/IFDSSignAnalysis.cpp
)

set(IFDS_SOURCES ${IFDS_FRAMEWORK_SOURCES} ${IFDS_CLIENT_SOURCES})

add_library(IFDS STATIC ${IFDS_SOURCES})

target_link_libraries(IFDS Annotation)

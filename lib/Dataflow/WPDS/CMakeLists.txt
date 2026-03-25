# WPDS-based interprocedural dataflow analysis implementation

# Framework core
set(WPDS_FRAMEWORK_SOURCES
  Core/DataFlowFacts.cpp
  Core/GenKillTransformer.cpp
  Solver/InterProceduralDataFlowEngine.cpp
)

# Client analyses
set(WPDS_CLIENT_SOURCES
  Clients/WPDSUninitializedVariables.cpp
  Clients/WPDSConstantPropagation.cpp
  Clients/WPDSLivenessAnalysis.cpp
  Clients/WPDSTaintAnalysis.cpp
)

set(WPDS_SOURCES ${WPDS_FRAMEWORK_SOURCES} ${WPDS_CLIENT_SOURCES})

add_library(WPDSDataFlow STATIC ${WPDS_SOURCES})

# Optional witness tracing support in the WPDS solver (requires WITNESS_TRACE).
option(WPDS_WITNESS_TRACE "Enable WPDS witness tracing in WPDSDataFlow" OFF)
if(WPDS_WITNESS_TRACE)
  target_compile_definitions(WPDSDataFlow PRIVATE WITNESS_TRACE)
endif()

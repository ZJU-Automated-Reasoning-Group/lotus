# LotusAA sources organized by subsystem

# Memory Model: Core abstractions for memory objects and points-to graphs
set(LOTUSAA_MEMORY_MODEL_SOURCES
  MemoryModel/MemObject.cpp
  MemoryModel/ObjectLocator.cpp
  MemoryModel/PointsToGraph.cpp
)

# Engine: Analysis drivers and transfer functions
set(LOTUSAA_ENGINE_SOURCES
  # Core drivers
  Engine/IntraProceduralAnalysis.cpp
  Engine/InterProceduralPass.cpp
  
  # Transfer functions for different constructs
  Engine/TransferFunctions/PointerInstructions.cpp
  Engine/TransferFunctions/StrongUpdates.cpp
  Engine/TransferFunctions/BasicOps.cpp
  Engine/TransferFunctions/CallHandling.cpp
  Engine/TransferFunctions/SummaryBuilder.cpp
  Engine/TransferFunctions/CallGraphSolver.cpp
)

# Support: Utilities, data structures, and configuration
set(LOTUSAA_SUPPORT_SOURCES
  Support/Utilities.cpp
  Support/LotusConfig.cpp
  Support/CallGraphState.cpp
  Support/FunctionPointerResults.cpp
)

set(LOTUSAA_SOURCES
  ${LOTUSAA_MEMORY_MODEL_SOURCES}
  ${LOTUSAA_ENGINE_SOURCES}
  ${LOTUSAA_SUPPORT_SOURCES}
)

# Build as STATIC library for linking into executables
add_library(LotusAA STATIC ${LOTUSAA_SOURCES})


target_link_libraries(LotusAA PUBLIC
  LLVMAnalysis
  AliasSpecManager
  CanaryPlatform
  CanaryGSA
  CanaryParallel
)

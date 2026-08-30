# Program Dependency Graph (PDG) CMake configuration
#
# This file configures the build for the PDG module, which implements
# a comprehensive program dependency graph for LLVM-based analysis.
# 

add_library(CanaryPDG STATIC
  Core/CallWrapper.cpp
  Core/ControlDependencyGraph.cpp
  Core/DataDependencyGraph.cpp
  Core/FunctionWrapper.cpp
  Core/Graph.cpp
  Core/PDGCallGraph.cpp
  Core/PDGNode.cpp
  Core/ProgramDependencyGraph.cpp
  Core/Tree.cpp
  QueryLanguage/Execution/CypherExecutor.cpp
  QueryLanguage/Execution/CypherResult.cpp
  QueryLanguage/Parser/CypherParser.cpp
  Analysis/Query.cpp
  Analysis/QueryCore.cpp
  Analysis/QuerySupport.cpp
  Analysis/SliceQuery.cpp
  Analysis/DependenceQuery.cpp
  Analysis/DataFlowQuery.cpp
  Analysis/TransformQuery.cpp
  Analysis/DiffQuery.cpp
  Analysis/SummaryQuery.cpp
  Analysis/ImpactQuery.cpp
  Analysis/ResourceFlowQuery.cpp
  Analysis/PropertySpec.cpp
  Support/DebugInfoUtils.cpp
  Support/GraphWriter.cpp
  Support/PDGUtils.cpp
)

target_link_libraries(CanaryPDG PUBLIC CanaryControlDependence)

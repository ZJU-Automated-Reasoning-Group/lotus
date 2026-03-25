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
  Analysis/CypherQuery.cpp
  Analysis/PDGQuery.cpp
  Analysis/SummaryQuery.cpp
  Analysis/ImpactQuery.cpp
  Analysis/ResourceFlowQuery.cpp
  Analysis/PropertyBasedSlicing.cpp
  Support/DebugInfoUtils.cpp
  Support/GraphWriter.cpp
  Support/PDGUtils.cpp
) 

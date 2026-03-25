# Mono-based dataflow analysis implementation

set(MONO_SOURCES
  Analyses/Intra/IntraAvailableExpressions.cpp
  Analyses/Intra/IntraConstantPropagation.cpp
  Analyses/Intra/IntraFullConstantPropagation.cpp
  Analyses/Intra/IntraLiveVariables.cpp
  Analyses/Intra/IntraReachable.cpp
  Analyses/Intra/IntraReachingDefinitions.cpp
  Analyses/Intra/IntraSolverTest.cpp
  Analyses/Intra/IntraUninitVariables.cpp
  Analyses/Inter/InterConstantPropagation.cpp
  Analyses/Inter/InterFullConstantPropagation.cpp
  Analyses/Inter/InterSolverTest.cpp
  Analyses/Inter/InterTaintAnalysis.cpp
)

add_library(MONODataFlow STATIC ${MONO_SOURCES})

  

# Header-only framework components live under include/Dataflow/Mono/.
# MONODataFlow contains the compiled concrete analyses.

set(MONO_SOURCES
  Analyses/Intra/AvailableExpressions.cpp
  Analyses/Intra/ConstantPropagation.cpp
  Analyses/Intra/FullConstantPropagation.cpp
  Analyses/Intra/LiveVariables.cpp
  Analyses/Intra/Reachability.cpp
  Analyses/Intra/ReachingDefinitions.cpp
  Analyses/Intra/SolverTest.cpp
  Analyses/Intra/UninitializedVariables.cpp
  Analyses/Inter/ConstantPropagation.cpp
  Analyses/Inter/FullConstantPropagation.cpp
  Analyses/Inter/SolverTest.cpp
  Analyses/Inter/Taint.cpp
)

add_library(MONODataFlow STATIC ${MONO_SOURCES})

# APA-based (Algebraic Program Analysis) intraprocedural dataflow solver.
#
# The solver itself is template-based and lives in include/Dataflow/APA/.
# This target exists mainly for uniform build integration and compilation checks.

set(APA_SOURCES
  Elimination.cpp
  Passes/EliminationPasses.cpp
  Analyses/Intra/IntraReachable.cpp
  Analyses/Intra/IntraConstantPropagation.cpp
  Analyses/Intra/IntraReachingDefinitions.cpp
  Analyses/Intra/IntraAvailableExpressions.cpp
  Analyses/Intra/IntraUninitVariables.cpp
  Analyses/Intra/IntraLiveVariables.cpp
  Analyses/Intra/IntraVeryBusyExpressions.cpp
  Analyses/Intra/IntraNonNull.cpp
)

add_library(APADataFlow STATIC ${APA_SOURCES})

# NPA sources
set(NPA_SOURCES
  Domains/AffineRelationDomain.cpp
  Domains/BitVectorDomain.cpp
  Domains/GenKillDomain.cpp
  Domains/PredicateRelationDomain.cpp
  Domains/TaintTransferDomain.cpp

  Analyses/BitVectorSolver.cpp
  Analyses/Intraprocedural/LiveVariables.cpp
  Analyses/Interprocedural/InterproceduralAffineEqualities.cpp
  Analyses/Interprocedural/InterproceduralConstantPropagation.cpp
  Analyses/Interprocedural/InterproceduralIntervalAnalysis.cpp
  Analyses/Interprocedural/InterproceduralLiveVariables.cpp
  Analyses/Interprocedural/InterproceduralNullability.cpp
  Analyses/Intraprocedural/ReachingDefinitions.cpp
  Analyses/Intraprocedural/ReachableBlocks.cpp
  Analyses/Interprocedural/InterproceduralMaybeUninitialized.cpp
  Analyses/Interprocedural/InterproceduralRD.cpp
  Analyses/Interprocedural/InterproceduralTaint.cpp
)

add_library(NPADataFlow STATIC ${NPA_SOURCES})
target_link_libraries(NPADataFlow PUBLIC CanaryCUDD CanaryParallel)

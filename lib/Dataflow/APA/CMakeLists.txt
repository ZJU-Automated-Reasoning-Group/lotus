# APA-based (Algebraic Program Analysis) dataflow solver.
#
# Header-only framework components live under include/Dataflow/APA/.
# APADataFlow contains compiled domains, LLVM analyses, and pass wrappers.

set(APA_SOURCES
  Domains/AffineRelationDomain.cpp
  Passes/EliminationPasses.cpp
  Analyses/Intra/Reachability.cpp
  Analyses/Intra/ConstantPropagation.cpp
  Analyses/Intra/ReachingDefinitions.cpp
  Analyses/Intra/AvailableExpressions.cpp
  Analyses/Intra/UninitializedVariables.cpp
  Analyses/Intra/LiveVariables.cpp
  Analyses/Intra/Lockset.cpp
  Analyses/Intra/VeryBusyExpressions.cpp
  Analyses/Intra/NonNull.cpp
  Analyses/Intra/Sign.cpp
  Analyses/Inter/Reachability.cpp
  Analyses/Inter/ConstantPropagation.cpp
  Analyses/Inter/LiveVariables.cpp
  Analyses/Inter/Lockset.cpp
  Analyses/Inter/ReachingDefinitions.cpp
  Analyses/Inter/UninitializedVariables.cpp
  Analyses/Inter/AffineEqualities.cpp
)

add_library(APADataFlow STATIC ${APA_SOURCES})

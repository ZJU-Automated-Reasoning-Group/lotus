# Sifa: Symbolic Interpretation with Fluid Abstractions (Ultimate Library-Sifa port)
# Layout mirrors include/Verification/Sifa/ for clarity.
#
# Subdirs with sources:
#   Caches/     - ProcedureResourceCache, StarDagCache, TopsortCache
#   Cfg/        - Transition (ostream, etc.)
#   Fluid/      - explicit instantiations (NeverFluid)
#   Interpreter/ - explicit instantiations (DagInterpreter, IcfgInterpreter)
#   Procedure/  - ProcedureGraph, ProcedureGraphBuilder, ProcedureResources
#   RegexDag/   - explicit instantiations (RegexDag, BackwardClosedOverlay, RegexDagNode)
#   Statistics/ - SifaStats
#   Storage/    - explicit instantiations (MapBasedStorage)
#   Summarizers/ - explicit instantiations (FixpointLoopSummarizer)
#   SymAbs/     - SifaSymAbsDomain
#   Worklist/   - explicit instantiations (PriorityWorklist)
# Root: Sifa.cpp, SifaSymAbs.cpp, CallGraph.cpp
#
# Template impl in headers/.tpp; explicit instantiations in corresponding lib dirs.

set(SIFA_SOURCES
  # Call graph (interprocedural)
  CallGraph.cpp
  # Log
  Log/SifaLogger.cpp
  # Caches
  Caches/ProcedureResourceCache.cpp
  Caches/StarDagCache.cpp
  Caches/TopsortCache.cpp
  # Cfg
  Cfg/Transition.cpp
  # Domain (instruction-level block transfer)
  Domain/IntervalDomain.cpp
  Domain/OctagonDomain.cpp
  # Fluid (explicit instantiations)
  Fluid/Instances.cpp
  # Interpreter (explicit instantiations)
  Interpreter/Instances.cpp
  # Procedure (procedure graph + builder + resources)
  Procedure/ProcedureGraph.cpp
  Procedure/ProcedureGraphBuilder.cpp
  Procedure/ProcedureResources.cpp
  # RegexDag (explicit instantiations)
  RegexDag/Instances.cpp
  # Statistics
  Statistics/SifaStats.cpp
  # Storage (explicit instantiations)
  Storage/Instances.cpp
  # Summarizers (explicit instantiations)
  Summarizers/Instances.cpp
  # SymAbs (domain implementations)
  SymAbs/SifaSymAbsDomain.cpp
  # Worklist (explicit instantiations)
  Worklist/Instances.cpp
  # Root
  Sifa.cpp
  SifaSymAbs.cpp
)

add_library(SifaAnalysis STATIC ${SIFA_SOURCES})
target_link_libraries(SifaAnalysis PUBLIC SymAbsAIAnalysis)

install(TARGETS SifaAnalysis DESTINATION lib)

set(CanaryCFG_SOURCES
  CFGReachability.cpp
  CodeMetrics.cpp
  Dominator.cpp
  DominatorForest.cpp
  DominatorNode.cpp
  SortTopo.cpp
  TopologicalOrder.cpp
)
add_library(CanaryCFG STATIC ${CanaryCFG_SOURCES})
target_link_libraries(CanaryCFG CanaryLLVMUtils)

add_subdirectory(PE)

add_library(CanaryOptimization STATIC
  AInliner.cpp
  ModuleOptimizer.cpp
  LICM.cpp
  IP/IPDeadStoreElimination.cpp
  IP/IPRedundantLoadElimination.cpp
  IP/IPStoreSinking.cpp
  IP/IPStoreToLoadForwarding.cpp
)
add_subdirectory(SWPrefetching)
target_link_libraries(CanaryOptimization CanaryLLVMUtils)

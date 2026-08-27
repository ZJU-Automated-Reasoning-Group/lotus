add_library(CanaryICFG STATIC
  CallGraph.cpp
  GraphAnalysis.cpp
  ICFGBuilder.cpp
  ICFG.cpp
)

target_link_libraries(CanaryICFG PUBLIC
  LLVMAnalysis
  LLVMCore)

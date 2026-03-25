add_library(CanaryDyckAA STATIC
        AAAnalyzer.cpp
        DyckAliasAnalysis.cpp
        DyckCallGraph.cpp
        DyckCallGraphNode.cpp
        DyckGraph.cpp
        DyckGraphNode.cpp
        DyckModRefAnalysis.cpp
        DyckValueFlowAnalysis.cpp
        DyckVFG.cpp
        MRAnalyzer.cpp
)

target_link_libraries(CanaryDyckAA CanaryLLVMUtils CanaryParallel CanaryCFG AliasSpecManager)

add_library(CanaryNisse STATIC
        Edge.cpp
        UnionFind.cpp
        NisseAnalysis.cpp
        NissePass.cpp
        NissePlugin.cpp
        prof.c
)

target_link_libraries(CanaryNisse
        LLVMAnalysis
        LLVMCore
        LLVMSupport
)

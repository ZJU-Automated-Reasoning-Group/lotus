add_library(CanaryClassicalCFL STATIC
    CNF.cpp
    Grammar.cpp
    Graph.cpp
    Relation.cpp
    HybridForest.cpp
    Validation.cpp
    Solver.cpp
    SCSolver.cpp
)

target_link_libraries(CanaryClassicalCFL PUBLIC
    LLVMSupport
)

add_library(CanaryClassicalCFLAlias STATIC
    Alias.cpp
    LLVMAliasAnalysis.cpp
)

target_link_libraries(CanaryClassicalCFLAlias PUBLIC
    CanaryClassicalCFL
    AserPTA
    LLVMCore
)

add_library(CanaryClassicalCFLSVFG STATIC
    SVFGAdapter.cpp
    SVFGPreparation.cpp
)

target_link_libraries(CanaryClassicalCFLSVFG PUBLIC
    CanaryClassicalCFL
    SVFG
    CanaryICFG
)

add_library(CanaryClassicalCFL STATIC
    Core/Grammar.cpp
    Core/Graph.cpp
    Core/Relation.cpp
    Core/Validation.cpp
    Solvers/Reachability.cpp
    Solvers/TransitiveClosure.cpp
    Solvers/ConstraintGrounding.cpp
)

target_link_libraries(CanaryClassicalCFL PUBLIC
    LLVMSupport
)

add_library(CanaryClassicalCFLAlias STATIC
    Clients/Alias/Alias.cpp
    Clients/Alias/LLVMAliasAnalysis.cpp
)

target_link_libraries(CanaryClassicalCFLAlias PUBLIC
    CanaryClassicalCFL
    AserPTA
    LLVMCore
)

add_library(CanaryClassicalCFLSVFG STATIC
    Clients/ValueFlow/ValueFlowClient.cpp
    Clients/ValueFlow/SVFGPreparation.cpp
)

target_link_libraries(CanaryClassicalCFLSVFG PUBLIC
    CanaryClassicalCFL
    SVFG
    CanaryICFG
)

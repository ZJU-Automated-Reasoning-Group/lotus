add_library(CanaryClassicalCFL STATIC
    Core/Grammar.cpp
    Core/Graph.cpp
    Core/Relation.cpp
    Core/RecursiveStateMachine.cpp
    Core/Validation.cpp
    Solvers/SolverSession.cpp
    Solvers/ConstraintGrounding.cpp
    Solvers/Engines/TransitiveClosure.cpp
    Solvers/Engines/POCR/ClientGrammars.cpp
    Solvers/Engines/POCR/FullyOrderedClosure.cpp
    Solvers/Engines/POCR/PairedTreeClosure.cpp
    Solvers/Engines/POCR/SpecializedEngines.cpp
    Solvers/Preprocessing/GraphSimplification.cpp
    Solvers/Preprocessing/RSMFoldability.cpp
)

target_link_libraries(CanaryClassicalCFL PUBLIC
    LLVMSupport
)

add_library(CanaryClassicalCFLAliasClient STATIC
    Clients/Alias/AliasClient.cpp
    Clients/Alias/LLVMAliasAnalysis.cpp
)

target_link_libraries(CanaryClassicalCFLAliasClient PUBLIC
    CanaryClassicalCFL
    AserPTA
    LLVMCore
)

add_library(CanaryClassicalCFLValueFlowClient STATIC
    Clients/ValueFlow/ValueFlowClient.cpp
    Clients/ValueFlow/SVFGPreparation.cpp
)

target_link_libraries(CanaryClassicalCFLValueFlowClient PUBLIC
    CanaryClassicalCFL
    SVFG
    CanaryICFG
)

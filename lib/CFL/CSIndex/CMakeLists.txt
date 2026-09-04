add_library(CanaryCSIndexFLARE STATIC
    FLARE/BitVector.cpp
    FLARE/Grail/Index.cpp
    FLARE/Grail/TCSEstimator.cpp
    FLARE/Graph.cpp
    FLARE/GraphAlgorithms.cpp
    FLARE/Index.cpp
    FLARE/PathTree/DataCompression.cpp
    FLARE/PathTree/Index.cpp
    FLARE/PathTree/Query.cpp
    FLARE/PathTree/WeightedGraph.cpp
    FLARE/PathTree/WeightedGraphAlgorithms.cpp
    FLARE/ReachBackbone.cpp
    FLARE/Tabulation/Parallel.cpp
    FLARE/Tabulation/Sequential.cpp
)

target_compile_features(CanaryCSIndexFLARE PUBLIC cxx_std_17)
target_link_libraries(CanaryCSIndexFLARE
    PUBLIC
        CanaryParallel
        CanaryPlatform)

add_library(CanaryCSIndexSCS STATIC
    SCS/Graph.cpp
    SCS/Index.cpp
    SCS/PolicyAutomaton.cpp
)

target_compile_features(CanaryCSIndexSCS PUBLIC cxx_std_17)
target_link_libraries(CanaryCSIndexSCS PUBLIC CanaryCSIndexFLARE)

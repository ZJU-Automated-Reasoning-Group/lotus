add_library(CanaryInterleavedDyckCore STATIC
    BidirectedDyck.cpp
    Graph.cpp
    UnaryGraph.cpp
)

target_compile_features(CanaryInterleavedDyckCore PUBLIC cxx_std_17)

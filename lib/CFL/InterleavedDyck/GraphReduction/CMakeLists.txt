add_library(CanaryInterleavedDyckGraphReduction STATIC
    CFLGraph.cpp
)

target_compile_features(CanaryInterleavedDyckGraphReduction PUBLIC cxx_std_17)

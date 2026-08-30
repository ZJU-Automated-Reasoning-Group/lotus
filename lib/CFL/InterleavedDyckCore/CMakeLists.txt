add_library(CanaryInterleavedDyckCore STATIC
    Graph.cpp
)

target_compile_features(CanaryInterleavedDyckCore PUBLIC cxx_std_17)

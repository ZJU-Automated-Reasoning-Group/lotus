add_library(CanaryInterleavedDyck STATIC
    Graph.cpp
    Solver.cpp
)

target_link_libraries(CanaryInterleavedDyck
    PUBLIC
        MutualRefinement)

target_compile_features(CanaryInterleavedDyck PUBLIC cxx_std_17)

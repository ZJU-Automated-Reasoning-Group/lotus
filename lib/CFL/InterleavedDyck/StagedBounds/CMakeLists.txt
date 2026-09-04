add_library(CanaryInterleavedDyckStagedBounds STATIC
    Solver.cpp
)

target_link_libraries(CanaryInterleavedDyckStagedBounds
    PUBLIC
        CanaryInterleavedDyckCore
        CanaryInterleavedDyckMutualRefinement)

target_compile_features(CanaryInterleavedDyckStagedBounds PUBLIC cxx_std_17)

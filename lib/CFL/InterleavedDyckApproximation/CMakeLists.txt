add_library(CanaryInterleavedDyckApproximation STATIC
    Solver.cpp
)

target_link_libraries(CanaryInterleavedDyckApproximation
    PUBLIC
        CanaryInterleavedDyckCore
        MutualRefinement)

target_compile_features(CanaryInterleavedDyckApproximation PUBLIC cxx_std_17)

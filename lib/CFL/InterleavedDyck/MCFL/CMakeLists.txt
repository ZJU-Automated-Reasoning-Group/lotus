add_library(CanaryInterleavedDyckMCFL STATIC
    Grammar.cpp
    Graph.cpp
    InterleavedDyck.cpp
    Solver.cpp
)

target_compile_features(CanaryInterleavedDyckMCFL PUBLIC cxx_std_17)

target_link_libraries(CanaryInterleavedDyckMCFL PUBLIC CanaryInterleavedDyckCore)

add_library(CanaryMCFL STATIC
    Grammar.cpp
    Graph.cpp
    InterleavedDyck.cpp
    Solver.cpp
)

target_compile_features(CanaryMCFL PUBLIC cxx_std_17)

target_link_libraries(CanaryMCFL PUBLIC CanaryInterleavedDyckCore)

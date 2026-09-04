add_library(CanaryInterleavedDyckUnary STATIC
    Adaptive.cpp
    FixedCounter.cpp
)

target_link_libraries(CanaryInterleavedDyckUnary
    PUBLIC
        CanaryInterleavedDyckCore)

target_compile_features(CanaryInterleavedDyckUnary PUBLIC cxx_std_17)

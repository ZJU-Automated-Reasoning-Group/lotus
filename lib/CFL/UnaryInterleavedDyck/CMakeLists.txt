add_library(CanaryUnaryInterleavedDyck STATIC
    Adaptive.cpp
    FixedCounter.cpp
)

target_link_libraries(CanaryUnaryInterleavedDyck
    PUBLIC
        CanaryInterleavedDyckCore)

target_compile_features(CanaryUnaryInterleavedDyck PUBLIC cxx_std_17)

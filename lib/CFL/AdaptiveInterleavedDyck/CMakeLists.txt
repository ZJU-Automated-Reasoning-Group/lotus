add_library(CanaryAdaptiveInterleavedDyck STATIC
    AdaptiveInterleavedDyck.cpp
)

target_link_libraries(CanaryAdaptiveInterleavedDyck
    PUBLIC
        CanaryInterleavedDyckCore)

target_compile_features(CanaryAdaptiveInterleavedDyck PUBLIC cxx_std_17)

add_subdirectory(Platform)
add_subdirectory(Random)
add_subdirectory(ADT)
add_subdirectory(Formats)
add_subdirectory(LLVM)
add_subdirectory(Parallel)

# CanarySupport: aggregate of Platform, Random, ADT, Formats (replaces old General)
add_library(CanarySupport INTERFACE)
target_link_libraries(CanarySupport INTERFACE
    CanaryPlatform
    CanaryRandom
    CanaryADT
    CanaryFormats
)

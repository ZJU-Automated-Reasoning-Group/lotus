add_library(CanaryInterDyckGraphReduce STATIC
    CFLGraph.cpp
)

target_compile_features(CanaryInterDyckGraphReduce PUBLIC cxx_std_17)

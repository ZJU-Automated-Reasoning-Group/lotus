# MutualRefinement - CFL Reachability with Mutual Refinement
# Context-free language reachability analysis with grammar refinement

# Source files for MutualRefinement library
set(MUTUALREFINEMENT_LIB_SOURCES
    CnfGrammar.cpp
    CnfGraph.cpp
    Hasher.cpp
    MutualRefinementMain.cpp
)

add_library(CanaryInterleavedDyckMutualRefinement STATIC
    ${MUTUALREFINEMENT_LIB_SOURCES})

target_compile_features(CanaryInterleavedDyckMutualRefinement
    PUBLIC cxx_std_17)

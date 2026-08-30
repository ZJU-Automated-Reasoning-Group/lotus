# MutualRefinement - CFL Reachability with Mutual Refinement
# Context-free language reachability analysis with grammar refinement

# Source files for MutualRefinement library
set(MUTUALREFINEMENT_LIB_SOURCES
    CnfGrammar.cpp
    CnfGraph.cpp
    Hasher.cpp
    MutualRefinementMain.cpp
)

# Create MutualRefinement static library
add_library(MutualRefinement STATIC ${MUTUALREFINEMENT_LIB_SOURCES})

# SVFG Library
# Production-ready Sparse Value-Flow Graph with AserPTA integration

# Sources
set(SVFG_SOURCES
    MemoryRegionPartitioner.cpp
    SVFG.cpp
    SVFGBuilder.cpp
    SVFGBuilderNodes.cpp
    SVFGBuilderEdges.cpp
    SVFGBuilderMemorySSA.cpp
    SVFGSerializer.cpp
    SVFGOPT.cpp
    SVFGStats.cpp
)

# Create library
add_library(SVFG STATIC ${SVFG_SOURCES} ${SVFG_HEADERS})

# Link dependencies
# AserPTA is the default pointer analysis (lib/Alias/InclusionBased/AserPTA)
target_link_libraries(SVFG PUBLIC
    CanaryICFG
    AserPTA
    LLVMCore
    LLVMSupport
)

# Alias analysis tools

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Core
  Coroutines
  IRReader
  Target
)

# AserPTA Pointer Analysis Tool
add_executable(aser-aa aser-aa.cpp)
target_link_libraries(aser-aa PRIVATE
  AserPTA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Sea-DSA Memory Graph Analysis Tool
add_executable(sea-dsa-dg sea-dsa-dg.cpp)
target_link_libraries(sea-dsa-dg PRIVATE
  SeaDsaAnalysis
  CanaryAliasCLIUtils
)

# Sea-DSA Advanced Analysis Tool
add_executable(seadsa-tool seadsa-tool.cpp)
target_link_libraries(seadsa-tool PRIVATE
  SeaDsaAnalysis
  CanaryAliasCLIUtils
)

# FPA (Function pointer analysis) Tool
add_executable(fpa fpa.cpp)
target_link_libraries(fpa PRIVATE
  CanaryFPA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# DFPA (Demand-refined function pointer analysis) Tool
add_executable(dfpa dfpa.cpp)
target_link_libraries(dfpa PRIVATE
  CanaryDFPA
  AserPTA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# LotusAA Pointer Analysis Tool
add_executable(lotus-aa lotus-aa.cpp)
target_link_libraries(lotus-aa PRIVATE
  LotusAA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Andersen's Pointer Analysis Tool
add_executable(sparrow-aa sparrow-aa.cpp)
target_link_libraries(sparrow-aa PRIVATE
  Andersen
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# DyckAA Pointer Analysis Tool
add_executable(dyck-aa dyck-aa.cpp)
target_link_libraries(dyck-aa PRIVATE
  CanaryDyckAA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# TPA (semi-sparse pointer analysis) Tool
add_executable(tpa tpa.cpp)
target_link_libraries(tpa PRIVATE
  FSCSPointerAnalysis
  TPATransforms
  TPAUtil
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Call Graph Construction Tool
add_executable(call-graph call-graph.cpp)
target_link_libraries(call-graph PRIVATE
  CanaryDyckAA
  LotusAA
  CanaryFPA
  CanaryDFPA
  AserPTA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Dynamic alias analysis
option(BUILD_DYNAA "Build dynamic alias analyses" OFF)

# Dynamic Alias Analysis tools (only built if BUILD_DYNAA is enabled)
if(BUILD_DYNAA)
  add_subdirectory(dynaa)
endif()

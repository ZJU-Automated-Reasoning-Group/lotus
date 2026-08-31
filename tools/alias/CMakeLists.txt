# Alias analysis tools

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Core
  Coroutines
  IRReader
  Target
)

# AserPTA Pointer Analysis Tool
add_executable(lotus-alias-aser-aa lotus-alias-aser-aa.cpp)
target_link_libraries(lotus-alias-aser-aa PRIVATE
  AserPTA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

add_executable(lotus-alias-fspta lotus-alias-fspta.cpp)
target_link_libraries(lotus-alias-fspta PRIVATE
  FlowSensitivePTA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Sea-DSA Memory Graph Analysis Tool
add_executable(lotus-alias-sea-dsa-dg lotus-alias-sea-dsa-dg.cpp)
target_link_libraries(lotus-alias-sea-dsa-dg PRIVATE
  SeaDsaAnalysis
  CanaryAliasCLIUtils
)

# Sea-DSA Advanced Analysis Tool
add_executable(lotus-alias-seadsa-tool lotus-alias-seadsa-tool.cpp)
target_link_libraries(lotus-alias-seadsa-tool PRIVATE
  SeaDsaAnalysis
  CanaryAliasCLIUtils
)

# FPA (Function pointer analysis) Tool
add_executable(lotus-alias-fpa lotus-alias-fpa.cpp)
target_link_libraries(lotus-alias-fpa PRIVATE
  CanaryFPA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# LotusAA Pointer Analysis Tool
add_executable(lotus-alias-lotus-aa lotus-alias-lotus-aa.cpp)
target_link_libraries(lotus-alias-lotus-aa PRIVATE
  LotusAA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Andersen's Pointer Analysis Tool
add_executable(lotus-alias-sparrow-aa lotus-alias-sparrow-aa.cpp)
target_link_libraries(lotus-alias-sparrow-aa PRIVATE
  Andersen
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# DyckAA Pointer Analysis Tool
add_executable(lotus-alias-dyck-aa lotus-alias-dyck-aa.cpp)
target_link_libraries(lotus-alias-dyck-aa PRIVATE
  CanaryDyckAA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# TPA (semi-sparse pointer analysis) Tool
add_executable(lotus-alias-tpa lotus-alias-tpa.cpp)
target_link_libraries(lotus-alias-tpa PRIVATE
  FSCSPointerAnalysis
  TPATransforms
  TPAUtil
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Call Graph Construction Tool
add_executable(lotus-alias-call-graph lotus-alias-call-graph.cpp)
target_link_libraries(lotus-alias-call-graph PRIVATE
  CanaryDyckAA
  LotusAA
  CanaryFPA
  CanaryDFPA
  AserPTA
  CanaryAliasCLIUtils
  ${LLVM_LINK_COMPONENTS}
)

# Dynamic Alias Analysis tools (only built if LOTUS_ENABLE_DYNAA is enabled)
if(LOTUS_ENABLE_DYNAA)
  add_subdirectory(dynaa)
endif()

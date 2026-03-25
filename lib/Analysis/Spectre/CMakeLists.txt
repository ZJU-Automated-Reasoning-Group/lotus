set(Spectre_SOURCES
  CacheModel.cpp
  CacheSpecuAnalysis.cpp
)
add_library(Spectre STATIC ${Spectre_SOURCES})
target_link_libraries(Spectre PRIVATE LLVMAnalysis)

add_library(CanaryFuzzing STATIC
  TargetGeneration.cpp
)
target_compile_definitions(CanaryFuzzing PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(CanaryFuzzing PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(CanaryFuzzing PUBLIC CanaryReport)

add_subdirectory(Analysis)
#add_definitions(AFLGoCompiler) # tbd
#add_subdirectoryAFLGoLinker)   # tbd

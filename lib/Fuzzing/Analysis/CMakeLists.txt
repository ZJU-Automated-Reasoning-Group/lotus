add_library(Analysis 
TargetDetection.cpp DAFL.cpp 
FunctionDistance.cpp
BasicBlockDistance.cpp 
ExtendedCallGraphAnalysis.cpp 
AnalysisPrinter.cpp)

set_property(TARGET Analysis PROPERTY POSITION_INDEPENDENT_CODE TRUE)
target_compile_definitions(Analysis PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(
  Analysis
  PUBLIC ${CMAKE_SOURCE_DIR}/include
  )
target_link_libraries(Analysis CanaryDyckAA)

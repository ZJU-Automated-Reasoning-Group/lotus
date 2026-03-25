# to fix  
add_llvm_library(
  ${AFLGO_LINKER_PLUGIN_NAME}
  MODULE
  DAFL.cpp
  DistanceInstrumentation.cpp
  DuplicateTargetRemoval.cpp
  TargetInjectionFixup.cpp
  FunctionDistanceInstrumentation.cpp
  Plugin.cpp)
target_compile_definitions(${AFLGO_LINKER_PLUGIN_NAME}
                           PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(${AFLGO_LINKER_PLUGIN_NAME}
                           PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(${AFLGO_LINKER_PLUGIN_NAME} PRIVATE Analysis)

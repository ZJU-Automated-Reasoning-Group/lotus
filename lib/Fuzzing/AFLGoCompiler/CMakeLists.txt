
# to fix  

add_llvm_library(${AFLGO_COMPILER_PLUGIN_NAME} MODULE TargetInjection.cpp Plugin.cpp)
target_compile_definitions(${AFLGO_COMPILER_PLUGIN_NAME} PRIVATE ${LLVM_DEFINITIONS})
target_include_directories(${AFLGO_COMPILER_PLUGIN_NAME} PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(${AFLGO_COMPILER_PLUGIN_NAME} PRIVATE Analysis)

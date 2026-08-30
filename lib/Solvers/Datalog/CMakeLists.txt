add_library(LotusDatalog STATIC
  Core/Context.cpp
  Core/Rule.cpp
  Core/Analyzer.cpp
  Core/Planner.cpp
  Runtime/KernelCompiler.cpp
  Runtime/Executor.cpp
  Runtime/RelationStore.cpp
  Runtime/Scheduler.cpp
  Semantic/SemanticProgram.cpp)

target_include_directories(LotusDatalog PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

llvm_map_components_to_libnames(DATALOG_KERNEL_LLVM_LIBS
  Core
  OrcJIT
  ExecutionEngine
  native)

target_link_libraries(LotusDatalog PRIVATE ${DATALOG_KERNEL_LLVM_LIBS})

set_target_properties(LotusDatalog PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

llvm_map_components_to_libnames(DATALOG_FRONTEND_LLVM_LIBS Support)

add_library(LotusDatalogFrontend STATIC
  Frontend/Dispatch.cpp
  Frontend/Json.cpp
  Frontend/Lowering.cpp
  Frontend/Lotus.cpp
  Frontend/Z3.cpp)

target_link_libraries(LotusDatalogFrontend PUBLIC
  LotusDatalog
  ${DATALOG_FRONTEND_LLVM_LIBS})

target_include_directories(LotusDatalogFrontend PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

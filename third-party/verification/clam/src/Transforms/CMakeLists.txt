add_library(ClamLlvmPasses STATIC
  InsertEntryPoint.cc
  LowerCstExpr.cc
  LowerSelect.cc
  LowerUnsignedICmp.cc
  RemoveUnreachableBlocksPass.cc
  MarkInternalInline.cc
  DevirtFunctions.cc
  DevirtFunctionsPass.cc
  ExternalizeFunctions.cc  
  ExternalizeAddressTakenFunctions.cc
  PromoteAssume.cc  
  PromoteMalloc.cc
  NondetInit.cc
  LoopPeeler.cc
  PropertyInstrumentation/MemoryCheck.cc
  PropertyInstrumentation/NullCheck.cc
  PropertyInstrumentation/UseAfterFreeCheck.cc  
)

llvm_map_components_to_libnames(LLVM_LIBS
  ipo 
  scalaropts 
  codegen 
  objcarcopts
  )

target_link_libraries(ClamLlvmPasses PUBLIC
  CanaryLLVMUtils
  SeaDsaAnalysis
  Lotus::ClamBackendConfig
  ## Needed if dynamic linking
  ${LLVM_LIBS})

target_include_directories(ClamLlvmPasses PUBLIC
  ${CMAKE_SOURCE_DIR}/include/Alias
)

install(TARGETS ClamLlvmPasses 
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib)

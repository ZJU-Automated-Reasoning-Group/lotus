add_library (seahorn.LIB STATIC
  LoadCrab.cc
  LiveSymbols.cc
  SymStore.cc
  UfoOpSem.cc
  InterMemPreProc.cc
  ClpOpSem.cc
  HornifyModule.cc
  HornifyFunction.cc
  FlatHornifyFunction.cc
  IncHornifyFunction.cc
  HornWrite.cc
  HornSolver.cc
  Houdini.cc
  HornModelConverter.cc
  HornDbModel.cc
  PredicateAbstraction.cc
  GuessCandidates.cc
  HornCex.cc
  CexHarness.cc
  CexExeGenerator.cc
  ClpWrite.cc
  HornClauseDB.cc
  HornClauseDBTransf.cc
  FiniteMapTransf.cc
  PathBmc.cc
  PathBmcBoolAbs.cc
  PathBmcMuc.cc
  PathBmcUtil.cc
  Bmc.cc
  SolverBmc.cc
  BmcPass.cc
  BvOpSem.cc
  # BvInt.cc
  MemSimulator.cc
  ZOption.cc
  BoogieWriter.cc
  BvOpSem2.cc
  BvOpSem2Mem.cc
  BvOpSem2MemRepr.cc
  BvOpSem2MemManager.cc
  BvOpSem2Allocators.cc
  BvOpSem2Alu.cc
  BvOpSem2ConstEval.cc
  BvOpSem2RawMemMgr.cc
  BvOpSem2FatMemMgr.cc
  BvOpSem2WideMemMgr.cc
  BvOpSem2TrackingRawMemMgr.cc
  BvOpSem2ExtraWideMemMgr.cc
  VCGen.cc
  DfCoiAnalysis.cc
  )

target_include_directories(seahorn.LIB PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include/Apps
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_BINARY_DIR}/include/Apps
)

# Link LLVM CodeGen library for IntrinsicLowering
llvm_map_components_to_libnames(llvm_codegen_libs codegen)
target_link_libraries(seahorn.LIB PRIVATE ${llvm_codegen_libs})

# Suppress deprecation warnings
target_compile_options(seahorn.LIB PRIVATE -Wno-deprecated-declarations)

add_subdirectory(Smt)
#if (HAVE_CLAM)
#  target_link_libraries (seahorn.LIB PRIVATE ${CLAM_LIBS})
#endif()


set_target_properties (seahorn.LIB PROPERTIES OUTPUT_NAME "seahorn")

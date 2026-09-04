set(SEA_ANALYSIS_SOURCES
  CanAccessMemory.cc
  CanFail.cc
  CutPointGraph.cc
  TopologicalOrder.cc
  WeakTopologicalOrder.cc
  CanReadUndef.cc
  GateAnalysis.cc
  ControlDependenceAnalysis.cc
  ClassHierarchyAnalysis.cc
  StaticTaint.cc
  SeaBuiltinsInfo.cc
)

if(HAVE_CLAM)
  list(APPEND SEA_ANALYSIS_SOURCES CrabAnalysis.cc)
endif()

add_library(SeaAnalysis STATIC ${SEA_ANALYSIS_SOURCES})

# Suppress deprecation warnings
target_compile_options(SeaAnalysis PRIVATE -Wno-deprecated-declarations)

target_link_libraries(SeaAnalysis PUBLIC
  CanaryLLVMUtils
)

if(HAVE_CLAM)
  target_link_libraries(SeaAnalysis PRIVATE ${CLAM_LIBS})
endif()

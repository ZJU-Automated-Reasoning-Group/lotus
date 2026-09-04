# AE (Abstract Execution) Checker Library
# Migrated from SVF's AE engine to Lotus.

set(AE_SOURCES
  AbstractState.cpp
  AbstractInterpretation.cpp
  AEDetector.cpp
  AbsExtAPI.cpp
  RelationSolver.cpp
  RelExeState.cpp
  ICFGWTO.cpp
  SVFIRWrapper.cpp
)

add_library(AE STATIC ${AE_SOURCES})

target_link_libraries(AE
  PUBLIC
    ${LLVM_LINK_COMPONENTS}
    CheckerFramework
    CanarySMT
    AserPTA
    ${Z3_LIBRARIES}
)

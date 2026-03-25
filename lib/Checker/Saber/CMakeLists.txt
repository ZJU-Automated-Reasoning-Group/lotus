# Saber Checker Library
# Migrated from SVF's SABER engine to Lotus

set(Saber_SOURCES
  SaberSVFGBuilder.cpp
  SrcSnkDDA.cpp
  SaberCondAllocator.cpp
  SaberOptions.cpp
  ProgSlice.cpp
  LeakChecker.cpp
  DoubleFreeChecker.cpp
  FileChecker.cpp
  SaberCheckerAPI.cpp
  Z3Expr.cpp
)

add_library(Saber STATIC ${Saber_SOURCES})
target_compile_definitions(Saber PRIVATE USE_Z3)
target_link_libraries(Saber
  PUBLIC
    SVFG
    CanaryICFG
    DDA
    CanaryReport
    ${Z3_LIBRARIES}
)

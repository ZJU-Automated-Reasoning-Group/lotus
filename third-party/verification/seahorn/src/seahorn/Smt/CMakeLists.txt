add_library (SeaSmt STATIC
  MarshalYices.cc
  Yices2SolverImpl.cc
  Yices2ModelImpl.cc
  ExprToZ.cc
  ZToExpr.cc
  ZToExprNoRec.cc
  ExprUtil.cc
  TypeChecker.cc
  HexDump.cc
  ExprMemMap.cc
  )

target_link_libraries(SeaSmt PRIVATE ${Z3_LIBRARY})
# Suppress deprecation warnings
target_compile_options(SeaSmt PRIVATE -Wno-deprecated-declarations)

if (YICES2_FOUND)
  target_link_libraries(SeaSmt PRIVATE ${YICES2_LIBRARY})
endif()


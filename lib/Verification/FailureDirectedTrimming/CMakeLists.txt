# Failure-directed program trimming (FSE'17) — split implementation

add_library(FailureDirectedTrimming STATIC
  Options.cpp
  Expr.cpp
  Codegen.cpp
  Z3QE.cpp
  SafetyConditions.cpp
  CloneAndWrap.cpp
  Pass.cpp
)

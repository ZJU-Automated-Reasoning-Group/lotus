add_library(CanaryControlDependence STATIC
  CompactClosure.cpp
  CompactDOD.cpp
  CompactNTSCD.cpp
  ControlClosure.cpp
  ControlDependence.cpp
  DOD.cpp
  NTSCD.cpp
  SCD.cpp)

target_link_libraries(CanaryControlDependence PUBLIC
  LLVMAnalysis
  LLVMCore)

add_library(CanaryICFGControlDependence STATIC ICFGControlDependence.cpp)

target_link_libraries(CanaryICFGControlDependence PUBLIC
  CanaryControlDependence
  CanaryICFG)

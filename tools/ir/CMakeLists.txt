add_executable(lotus-ir-pdg-query
  lotus-ir-pdg-query.cpp
)

target_link_libraries(lotus-ir-pdg-query
  PRIVATE
  CanaryPDG
  CanaryCUDD
  CanaryAliasAnalysisWrapper
  LLVMIRReader
)

add_executable(lotus-ir-control-dependence
  lotus-ir-control-dependence.cpp
)

target_link_libraries(lotus-ir-control-dependence
  PRIVATE
  CanaryControlDependence
  LLVMIRReader
)

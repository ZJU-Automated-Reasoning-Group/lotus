add_library(VerificationAnalysis STATIC
  CheckModule.cpp
  ClassifyInstructions.cpp
  ClassifyLoops.cpp
  CountInstr.cpp
  GetTestTargets.cpp
)

target_link_libraries(VerificationAnalysis
  LLVMCore
  LLVMSupport
  LLVMAnalysis
)

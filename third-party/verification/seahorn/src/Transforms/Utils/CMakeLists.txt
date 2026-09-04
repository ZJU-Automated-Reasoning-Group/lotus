add_library(SeaTransformsUtils STATIC
  AbstractMemory.cc
  DummyExitBlock.cc
  Local.cc
  MarkInternalInline.cc
  MarkInternalSpecialFunctions.cc
  NameValues.cc
  RemoveUnreachableBlocksPass.cc
  DummyMainFunction.cc
  EvalBranchSentinelPass.cc
  ExternalizeAddressTakenFunctions.cc
  ExternalizeFunctions.cc
  DevirtFunctions.cc
  DevirtFunctionsPass.cc
  SliceFunctions.cc
  Mem2Reg.cc
  OneAssumePerBlock.cc
  SliceFunctions.cc
  DebugVerifier.cc
  )

target_link_libraries(SeaTransformsUtils PUBLIC
  CanaryLLVMUtils
  CanaryTransform
)

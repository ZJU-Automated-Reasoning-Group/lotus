add_library(SeaInstrumentation STATIC
  MixedSemantics.cc
  NondetInit.cc
  MarkFnEntry.cc
  EnumVerifierCalls.cc
  StripLifetime.cc
  StripUselessDeclarations.cc
  KleeInternalize.cc
  WrapMem.cc
  RenameNondet.cc
  NullCheck.cc
  SimpleMemoryCheck.cc
  FatBufferBoundsCheck.cc
  BranchSentinelPass.cc
  GeneratePartialFnPass.cc
  )

target_link_libraries(SeaInstrumentation PUBLIC
  CanaryTransform
)

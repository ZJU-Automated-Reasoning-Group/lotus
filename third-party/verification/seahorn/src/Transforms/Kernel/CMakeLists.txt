add_library(SeaTransformsKernel STATIC
#add_library(SeaTransformsKernel DISABLE_LLVM_LINK_LLVM_DYLIB
  Acpi.cc
  DsaSwitchOps.cc
  FileOperations.cc
  I2CDriver.cc
  KernelSetup.cc
  Platform.cc

  Debug.cc
  Slicer.cc
  Device.cc
  Devm.cc
  Util.cc
  NondetMalloc.cc
  IntoBinary.cc
  AssumeNonNull.cc
  InitGlobalKrefs.cc
  SpecificFunction.cc
  ListOps.cc
  HandleInlineAsm.cc
  AssertKrefs.cc
  SetupEntrypoint.cc
)
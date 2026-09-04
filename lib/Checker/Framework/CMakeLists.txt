add_library(CheckerFramework STATIC
  CheckerTypes.cpp
  CheckerDiagnostic.cpp
  CheckerRegistry.cpp
  CheckerValidator.cpp
  CheckerSpecLoader.cpp
  CheckerDriver.cpp
  SARIF.cpp
  BugReport.cpp
  BugReportMgr.cpp
  BugTypes.cpp
  ReportOptions.cpp
  SuppressionManager.cpp
  Subcommands.cpp
)

target_link_libraries(CheckerFramework
  PUBLIC
    CanaryAliasAnalysisWrapper
    CanaryDebugInfo
    CanaryFormats
    LLVMIRReader
    LLVMSupport
)
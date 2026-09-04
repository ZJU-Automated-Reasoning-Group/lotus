set(PulseChecker_SOURCES
  Checker/PulseChecker.cpp
  Checker/PulseCheckerInstructions.cpp
  Checker/PulseCheckerSummary.cpp
  Checker/PulseCheckerUtils.cpp

  Core/PulseCallState.cpp
  Core/PulseFormula.cpp
  Core/PulseSubstitution.cpp

  Domain/PulseContradiction.cpp
  Domain/PulseDisjunctiveDomain.cpp
  Domain/PulseDomain.cpp
  Domain/PulseJoin.cpp
  Domain/PulseLoopAbstraction.cpp
  Domain/PulseNonDisjunctiveDomain.cpp
  Domain/PulseOperations.cpp
  Domain/PulseTaint.cpp
  Domain/PulseTaintConfig.cpp

  Interproc/PulseModels.cpp
  Interproc/PulseSpecialization.cpp
  Interproc/PulseSummary.cpp
  Interproc/PulseSummaryApplication.cpp
  Interproc/PulseTransitiveInfo.cpp

  Report/PulseDiagnostic.cpp
  Report/PulseLatentIssue.cpp
  Report/PulseLogger.cpp
  Report/PulseReport.cpp
)

add_library(PulseChecker STATIC ${PulseChecker_SOURCES})

target_link_libraries(PulseChecker
  PUBLIC
    CanaryAliasAnalysisWrapper
    CheckerFramework
    ${Z3_LIBRARIES}
)

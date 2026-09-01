# Core build surfaces
option(LOTUS_BUILD_EXAMPLES "Build examples" OFF)
option(LOTUS_BUILD_TESTS "Build tests" OFF)
option(LOTUS_ENABLE_COVERAGE
       "Instrument Lotus and its tests for LLVM source coverage" OFF)
set(LOTUS_COVERAGE_MINIMUM 0 CACHE STRING
    "Minimum total line coverage percentage required by coverage_report")

# Optional analysis and verification integrations.
option(LOTUS_ENABLE_CLAM "Enable CLAM abstract interpretation framework" OFF)
option(LOTUS_ENABLE_SEAHORN "Enable SeaHorn" OFF)
option(LOTUS_ENABLE_SMACK
       "Enable SMACK LLVM-to-Boogie verifier frontend" OFF)
option(LOTUS_ENABLE_SVF "Enable SVF" OFF)
option(LOTUS_USE_CCLYZER
       "Enable optional cclyzer++ alias analysis backend" OFF)

# Optional in-tree components
option(LOTUS_ENABLE_TYPE_QUALIFIER
       "Enable the opt-in TypeQualifier uninitialized-data checker" OFF)
option(LOTUS_ENABLE_FPSOLVE
       "Build the vendored FPsolve library under third-party/fpsolve" OFF)
option(LOTUS_ENABLE_WALI_OPENNWA
       "Build the vendored WALi/OpenNWA library under third-party/WALi-OpenNWA"
       OFF)
option(LOTUS_ENABLE_CFL "Build CFL reachability solvers" OFF)
option(LOTUS_ENABLE_CSR
       "Build the indexing context-sensitive reachability solver" OFF)
option(LOTUS_ENABLE_OWL "Build Owl SMT solver" OFF)
option(LOTUS_ENABLE_SMT_STABILIZER
       "Build the SMTStabilizer SMT-LIB normalization library and tool" OFF)
option(LOTUS_ENABLE_DYNAA "Build dynamic alias analyses" OFF)
option(LOTUS_ENABLE_HORN_ICE
       "Build ICE learning for CHC and Boogie" OFF)
option(LOTUS_ENABLE_SEAL
       "Build the Seal symbolic automata lifter under third-party/seal" OFF)

# Advanced toggles
option(LOTUS_DOWNLOAD_BOOST "Download and build Boost if not found" ON)
option(LOTUS_DOWNLOAD_CRAB "Download and build CRAB if not found" OFF)
option(LOTUS_SEAHORN_BUILD_32_BIT_RT "Build 32-bit SeaHorn runtime libraries"
       OFF)
option(LOTUS_SEADSA_ENABLE_SANITY_CHECKS
       "Enable Sea-dsa sanity checks" OFF)
option(LOTUS_WPDS_WITNESS_TRACE
       "Enable WPDS witness tracing in WPDSDataFlow" OFF)
option(LOTUS_EGRAPH_ENABLE_DOT
       "Enable DOT/Graphviz helpers in Lotus EGraph" ON)
option(LOTUS_EGRAPH_ENABLE_JSON
       "Enable JSON serialization helpers in Lotus EGraph" ON)

# User-provided dependency overrides
set(LOTUS_CUSTOM_BOOST_ROOT "" CACHE PATH
    "Path to a custom Boost installation")
set(LOTUS_CUSTOM_CRAB_ROOT "" CACHE PATH
    "Path to a custom CRAB installation")

function(_lotus_summary_bool label value)
  if(${value})
    message(STATUS "  ${label}: ON")
  else()
    message(STATUS "  ${label}: OFF")
  endif()
endfunction()

function(lotus_print_build_summary)
  message(STATUS "")
  message(STATUS "Lotus build summary")
  message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
  message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
  message(STATUS "  Binary dir: ${CMAKE_BINARY_DIR}")
  message(STATUS "  LLVM package: ${LLVM_PACKAGE_VERSION}")
  _lotus_summary_bool("Build tests" LOTUS_BUILD_TESTS)
  _lotus_summary_bool("Coverage instrumentation" LOTUS_ENABLE_COVERAGE)
  _lotus_summary_bool("Build examples" LOTUS_BUILD_EXAMPLES)
  message(STATUS "  Optional tool families:")
  _lotus_summary_bool("CFL tools" LOTUS_ENABLE_CFL)
  _lotus_summary_bool("CSR tool" LOTUS_ENABLE_CSR)
  _lotus_summary_bool("Owl SMT tool" LOTUS_ENABLE_OWL)
  _lotus_summary_bool("SMTStabilizer" LOTUS_ENABLE_SMT_STABILIZER)
  _lotus_summary_bool("DynAA tools" LOTUS_ENABLE_DYNAA)
  message(STATUS "  Optional integrations:")
  _lotus_summary_bool("CLAM" LOTUS_ENABLE_CLAM)
  _lotus_summary_bool("SeaHorn" LOTUS_ENABLE_SEAHORN)
  _lotus_summary_bool("SMACK" LOTUS_ENABLE_SMACK)
  _lotus_summary_bool("Horn-ICE" LOTUS_ENABLE_HORN_ICE)
  _lotus_summary_bool("Seal/Popeye" LOTUS_ENABLE_SEAL)
  _lotus_summary_bool("SVF" LOTUS_ENABLE_SVF)
  _lotus_summary_bool("Cclyzer++" LOTUS_USE_CCLYZER)
  message(STATUS "  Advanced toggles:")
  _lotus_summary_bool("TypeQualifier" LOTUS_ENABLE_TYPE_QUALIFIER)
  _lotus_summary_bool("FPsolve" LOTUS_ENABLE_FPSOLVE)
  _lotus_summary_bool("WALi/OpenNWA" LOTUS_ENABLE_WALI_OPENNWA)
  _lotus_summary_bool("Download Boost" LOTUS_DOWNLOAD_BOOST)
  _lotus_summary_bool("Download CRAB" LOTUS_DOWNLOAD_CRAB)
  _lotus_summary_bool("SeaDsa sanity checks" LOTUS_SEADSA_ENABLE_SANITY_CHECKS)
  _lotus_summary_bool("WPDS witness trace" LOTUS_WPDS_WITNESS_TRACE)
  _lotus_summary_bool("SeaHorn 32-bit runtime" LOTUS_SEAHORN_BUILD_32_BIT_RT)
  _lotus_summary_bool("EGraph DOT" LOTUS_EGRAPH_ENABLE_DOT)
  _lotus_summary_bool("EGraph JSON" LOTUS_EGRAPH_ENABLE_JSON)
endfunction()

# Unified checker front-end.

include(GNUInstallDirs)

llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Analysis
  IRReader
  Passes
)

add_executable(lotus-check
  CheckerOptions.cpp
  CheckerReport.cpp
  lotus-check-generic.cpp
  lotus-check-kint.cpp
  lotus-check-taint.cpp
  lotus-check-concur.cpp
  lotus-check-pulse.cpp
  lotus-check-fitx.cpp
  lotus-check-saber.cpp
  lotus-check-ae.cpp
  lotus-check-symex.cpp
)

target_link_libraries(lotus-check PRIVATE
  Annotation
  CanaryAliasAnalysisWrapper
  CanaryFuzzing
  CanaryKint
  CanarySymbolicExecution
  CheckerFramework
  ConcurrencyChecker
  FiTxChecker
  IFDS
  PulseChecker
  ${LLVM_LINK_COMPONENTS}
  ${Z3_LIBRARIES}
)

target_compile_definitions(lotus-check PRIVATE
  LOTUS_INSTALL_CHECKER_SPEC_DIR="${CMAKE_INSTALL_FULL_DATADIR}/lotus/checkers"
  LOTUS_SOURCE_CHECKER_SPEC_DIR="${PROJECT_SOURCE_DIR}/config/checkers"
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/config/checkers/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/lotus/checkers)
install(TARGETS lotus-check
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LOTUS_BUILD_TESTS)
  set(CHECKER_CLI_TEST_SCRIPT
      ${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/verify_cli.cmake)
  set(CHECKER_CLI_MINIMAL ${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/minimal.ll)
  set(CHECKER_CLI_TAINT ${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/taint.ll)
  set(CHECKER_CLI_TAINT_INVOKE
      ${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/taint_invoke.ll)
  set(CHECKER_CLI_AE_LEAK ${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/ae_leak.ll)

  function(add_checker_cli_test test_name expected_result expected_output)
    set(args ${ARGN})
    set(command
        ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:lotus-check>
        -DEXPECTED_RESULT=${expected_result}
        -DEXPECTED_OUTPUT=${expected_output}
        -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR})
    set(index 1)
    foreach(arg IN LISTS args)
      list(APPEND command -DCLI_ARG${index}=${arg})
      math(EXPR index "${index} + 1")
    endforeach()
    math(EXPR arg_count "${index} - 1")
    list(APPEND command -DCLI_ARG_COUNT=${arg_count})
    list(APPEND command -P ${CHECKER_CLI_TEST_SCRIPT})
    add_test(NAME ${test_name} COMMAND ${command})
  endfunction()

  function(add_checker_cli_test_without_output test_name expected_result
           expected_output unexpected_output)
    set(args ${ARGN})
    set(command
        ${CMAKE_COMMAND}
        -DTOOL=$<TARGET_FILE:lotus-check>
        -DEXPECTED_RESULT=${expected_result}
        -DEXPECTED_OUTPUT=${expected_output}
        -DUNEXPECTED_OUTPUT=${unexpected_output}
        -DWORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR})
    set(index 1)
    foreach(arg IN LISTS args)
      list(APPEND command -DCLI_ARG${index}=${arg})
      math(EXPR index "${index} + 1")
    endforeach()
    math(EXPR arg_count "${index} - 1")
    list(APPEND command -DCLI_ARG_COUNT=${arg_count})
    list(APPEND command -P ${CHECKER_CLI_TEST_SCRIPT})
    add_test(NAME ${test_name} COMMAND ${command})
  endfunction()

  add_checker_cli_test(checker_cli_ae_defaults_include_mem_leak 0
                       "Running memory leak checker"
                       --engine=ae --log-level=info ${CHECKER_CLI_MINIMAL})
  foreach(check IN ITEMS double-free memory-leak use-after-free)
    string(REPLACE "-" "_" check_name ${check})
    add_checker_cli_test(checker_cli_fitx_${check_name}_selection 0
                         "Checks: ${check}"
                         --engine=fitx --checks=${check} --log-level=info
                         ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_fitx_rejects_unknown_check 2
                       "unknown fitx check 'foobar'"
                       --engine=fitx --checks=foobar ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_concur_rejects_substring_check 2
                       "unknown concur check 'raceXYZ'"
                       --engine=concur --checks=raceXYZ ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_concur_analysis_respects_selection 0
                       "CONCURRENCY ANALYSIS RESULTS"
                       --engine=concur --concur.mode=analysis
                       --checks=data-race
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_concur_analysis_output_io_error 2
                       "Error writing concurrency analysis"
                       --engine=concur --concur.mode=analysis
                       --concur.output-format=json
                       --concur.output=${CMAKE_CURRENT_BINARY_DIR}/missing/analysis.json
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_taint_micro_bench_evaluates 0
                       "TP: 1  FP: 0  FN: 0"
                       --engine=taint --taint.micro-bench
                       --taint.expected-flows=${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/taint.expected
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_trims_function_lists 0
                       "\\[source\\] source"
                       --engine=taint --log-level=debug
                       "--taint.sources=source, source" --taint.sinks=sink
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_fail_on_findings 1
                       "Taint-Style Vulnerability"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --fail-on-findings ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_invoke_fail_on_findings 1
                       "Taint-Style Vulnerability"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --fail-on-findings ${CHECKER_CLI_TAINT_INVOKE})
  add_checker_cli_test_without_output(
      checker_cli_taint_ignores_operand_bundle 1 "via arg0" "arg1"
      --engine=taint --taint.sources=source --taint.sinks=sink
      --fail-on-findings
      ${PROJECT_SOURCE_DIR}/tests/regress/Checker/CLI/taint_operand_bundle.ll)
  add_checker_cli_test(checker_cli_taint_honors_min_score 0
                       "No findings"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --report-min-score=81 --fail-on-findings
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_writes_json 0
                       "JSON report written to"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --report-json=${CMAKE_CURRENT_BINARY_DIR}/taint-report.json
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_writes_sarif 0
                       "SARIF report written to"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --report-sarif=${CMAKE_CURRENT_BINARY_DIR}/taint-report.sarif
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_taint_rejects_missing_suppressions 2
                       "Error loading suppressions"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --suppressions=${CMAKE_CURRENT_BINARY_DIR}/missing-suppressions.json
                       ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_forwards_more_than_eight_arguments 0
                       "Analysis completed successfully"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --checks=taint-flow --taint.alias-analysis=dyck
                       --log-level=info --analysis-stats=false
                       --taint.micro-bench=false
                       --report-min-score=0 ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_fail_on_findings 1
                       "Memory Leak"
                       --engine=ae --checks=memory-leak --fail-on-findings
                       ${CHECKER_CLI_AE_LEAK})
  add_checker_cli_test(checker_cli_rejects_out_of_range_min_score 2
                       "--report-min-score must be in \\[0,100\\]"
                       --engine=ae --report-min-score=101
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_rejects_engine_after_option_sentinel 2
                       "Too many positional arguments"
                       --engine=taint --taint.sources=source --taint.sinks=sink
                       --
                       --engine=ae ${CHECKER_CLI_TAINT})
  add_checker_cli_test(checker_cli_report_io_error 2
                       "Error writing JSON report"
                       --engine=fitx --checks=use-after-free
                       --report-json=${CMAKE_CURRENT_BINARY_DIR}/missing/report.json
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_fitx_analysis_stats 0 "engine: fitx"
                       --engine=fitx --analysis-stats
                       --checks=use-after-free ${CHECKER_CLI_MINIMAL})
  foreach(old_option IN ITEMS --detector=all --measure --fitx-debug --async
                              --flex)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_fitx_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=fitx ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  foreach(old_option IN ITEMS --overflow --mem-leak --all -v --stats
                              --handle-recur=top --widen-delay=2
                              --strict-checkpoint=false --recursion-mode=top
                              --widen-delay-iterations=2
                              --checkpoint-policy=permissive)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_ae_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=ae ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_kint_rejects_old_check_all 2
                       "Unknown command line argument"
                       --engine=kint --check-all ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_kint_rejects_old_quiet 2
                       "Unknown command line argument"
                       --engine=kint --quiet ${CHECKER_CLI_MINIMAL})
  foreach(old_option IN ITEMS --kint-summary-mode=on
                              --kint-summary-timeout=5
                              --kint-summary-max-paths=64
                              --function-timeout=10
                              --robust-only-bugs=overflow
                              --log-to-stderr --log-to-file=log.txt
                              --function-timeout-seconds=10
                              --max-paths-per-function=10
                              --summary-timeout-seconds=5
                              --summary-max-paths=64 --summary-mode=on
                              --analyze-all-functions
                              --robust-reachability --robust-checks=all
                              --dump-ef-constraints=constraints.smt2
                              --robust-universal-unknown-loads
                              --robust-universal-external-globals
                              --robust-universal-inline-asm)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_kint_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=kint ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_concur_rejects_old_check_flag 2
                       "Unknown command line argument"
                       --engine=concur --check-data-races
                       ${CHECKER_CLI_MINIMAL})
  foreach(old_option IN ITEMS --analysis-only --analysis-json=analysis.json
                              --mode=analysis --output=analysis.txt
                              --output-format=json)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_concur_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=concur ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_pulse_rejects_old_stats 2
                       "Unknown command line argument"
                       --engine=pulse --pulse-stats ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_pulse_rejects_old_no_smt 2
                       "Unknown command line argument"
                       --engine=pulse --no-smt ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_pulse_rejects_flat_smt 2
                       "Unknown command line argument"
                       --engine=pulse --smt=off ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_taint_rejects_old_analysis 2
                       "Unknown command line argument"
                       --engine=taint --analysis=0 ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_taint_rejects_old_print_stats 2
                       "Unknown command line argument"
                       --engine=taint --print-stats ${CHECKER_CLI_MINIMAL})
  foreach(old_option IN ITEMS --aa=dyck --expected=flows.txt
                              --alias-analysis=dyck --sources=source
                              --sinks=sink --micro-bench
                              --expected-flows=flows.txt)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_taint_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=taint ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_generic_rejects_old_checker 2
                       "Unknown command line argument"
                       --engine=generic --checker=forbidden.system
                       ${CHECKER_CLI_MINIMAL})
  foreach(old_option IN ITEMS --category=security --spec-dir=specs)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_generic_rejects_flat_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=generic ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_checker_cli_test(checker_cli_saber_rejects_old_verbose 2
                       "Unknown command line argument"
                       --engine=saber --saber-verbose ${CHECKER_CLI_MINIMAL})
  foreach(old_option IN ITEMS --saber-full-svfg --saber-cxt-limit=3
                              --saber-max-step-wrapper=10
                              --saber-max-forward-items=10
                              --saber-z3-timeout=1000 --saber-dump-slice
                              --saber-validate-tests
                              --saber-collect-extret-globals=false
                              --full-svfg --context-limit=3
                              --max-steps-in-wrapper=10
                              --max-forward-items=10 --z3-timeout-ms=1000
                              --dump-slice
                              --collect-external-return-globals=false)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_saber_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=saber ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  foreach(old_option IN ITEMS --symex-bof --symex-dbz
                              --symex-checkers=bof --symex-uaf)
    string(MAKE_C_IDENTIFIER "${old_option}" old_option_name)
    add_checker_cli_test(checker_cli_symex_rejects_old_${old_option_name} 2
                         "Unknown command line argument"
                         --engine=symex ${old_option} ${CHECKER_CLI_MINIMAL})
  endforeach()
  add_test(
    NAME checker_cli_generic_finds_specs_outside_source_tree
    COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:lotus-check>
            -DEXPECTED_RESULT=0
            -DEXPECTED_OUTPUT=taint.getenv-system
            -DWORKING_DIRECTORY=/tmp
            -DCLI_ARG1=--engine=generic
            -DCLI_ARG2=--list-checkers
            -P ${CHECKER_CLI_TEST_SCRIPT})
  add_checker_cli_test(checker_cli_list_checkers_includes_taint 0
                       "taint-flow.*taint.*native"
                       --list-checkers)
  add_checker_cli_test(checker_cli_list_parameters_groups_engines 0
                       "engine.*saber"
                       --list-parameters)
  add_checker_cli_test(checker_cli_list_parameters_qualifies_options 0
                       "--kint.summary-mode"
                       --list-parameters)
  add_checker_cli_test(checker_cli_ae_help_uses_namespace 0
                       "--ae.recursion-mode"
                       --engine=ae --help)
  add_checker_cli_test(checker_cli_saber_help_uses_namespace 0
                       "--saber.solver-timeout-ms"
                       --engine=saber --help)
  add_checker_cli_test(checker_cli_rejects_wrong_engine_namespace 2
                       "Unknown command line argument '--kint.summary-mode'"
                       --engine=ae --kint.summary-mode=off
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_rejects_empty_checks 2
                       "--checks value must not be empty"
                       --engine=ae --checks= ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_rejects_duplicate_checks 2
                       "duplicate ae check 'memory-leak'"
                       --engine=ae --checks=memory-leak,memory-leak
                       ${CHECKER_CLI_MINIMAL})
  add_checker_cli_test(checker_cli_symex_help_lists_check_ids 0
                       "negative-array-index"
                       --engine=symex --help)
  add_checker_cli_test(checker_cli_list_checkers_rejects_missing_specs 2
                       "spec directory does not exist"
                       --engine=generic --list-checkers
                       --generic.spec-dir=${CMAKE_CURRENT_BINARY_DIR}/missing-specs)
  add_checker_cli_test(checker_cli_fitx_rejects_flat_report_candidates 2
                       "Unknown command line argument"
                       --engine=fitx --report-all-candidates
                       ${CHECKER_CLI_MINIMAL})
endif()

# The unified frontend directly references the Saber and AE entry points and
# option accessors, so normal static-library extraction retains the required
# objects without platform-specific whole-archive linker flags.
target_link_libraries(lotus-check PRIVATE Saber AE)

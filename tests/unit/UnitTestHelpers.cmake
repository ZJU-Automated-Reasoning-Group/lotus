set(LOTUS_UNIT_TEST_COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/../utils
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${LLVM_INCLUDE_DIRS})

set(LOTUS_TEST_BASE_LIBS
    GTest::gtest
    GTest::gtest_main
    LLVMAsmParser
    LLVMIRReader
    LLVMPasses)

set(LOTUS_TEST_HARNESS_BASE_LIBS
    GTest::gtest
    LLVMAsmParser
    LLVMIRReader
    LLVMPasses)

add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE ${LOTUS_TEST_BASE_LIBS})

add_library(lotus_test_harness_utils INTERFACE)
target_include_directories(lotus_test_harness_utils INTERFACE
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_harness_utils INTERFACE
    ${LOTUS_TEST_HARNESS_BASE_LIBS})

function(add_lotus_targeted_test test_name source_file)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs LINK_LIBS INCLUDE_DIRS)
    cmake_parse_arguments(LOTUS_TEST "${options}" "${oneValueArgs}"
        "${multiValueArgs}" ${ARGN})

    if(IS_ABSOLUTE ${source_file})
        set(source ${source_file})
    else()
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
    endif()

    add_executable(${test_name} ${source})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES}
        ${LOTUS_TEST_INCLUDE_DIRS})
    target_link_libraries(${test_name}
        lotus_test_utils
        ${LOTUS_TEST_LINK_LIBS})

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_concurrency_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            Concurrency
            CanaryParallel)
endfunction()

function(add_lotus_concurrency_test_suite test_name)
    add_executable(${test_name} ${ARGN})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_compile_definitions(${test_name} PRIVATE
        LOTUS_GTEST_NO_MAIN)
    target_link_libraries(${test_name}
        lotus_test_utils
        Concurrency
        CanaryParallel)

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_analysis_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryNullPointer
            CanaryDyckAA)
endfunction()

function(add_lotus_debug_info_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryDebugInfo)
endfunction()

function(add_lotus_spectre_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            Spectre)
endfunction()

function(add_lotus_crypto_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CryptoVerify)
endfunction()

function(add_lotus_checker_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            AE
            CanaryAliasAnalysisWrapper)
endfunction()

function(add_lotus_controlflow_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryICFG)
endfunction()

function(add_lotus_fuzzing_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryFuzzing
            CanaryReport)
endfunction()

function(add_lotus_ir_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryPDG
            CanaryICFG
            CanaryAliasAnalysisWrapper)
endfunction()

function(add_lotus_ir_test_suite test_name)
    add_executable(${test_name} ${ARGN})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_compile_definitions(${test_name} PRIVATE
        LOTUS_GTEST_NO_MAIN)
    target_link_libraries(${test_name}
        lotus_test_utils
        CanaryPDG
        CanaryICFG
        CanaryAliasAnalysisWrapper)

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_svfg_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryICFG
            SVFG
            CanaryAliasAnalysisWrapper)
endfunction()

function(add_lotus_svfg_test_suite test_name)
    add_executable(${test_name} ${ARGN})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_link_libraries(${test_name}
        lotus_test_utils
        CanaryICFG
        SVFG
        CanaryAliasAnalysisWrapper)

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_gsa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryGSA)
endfunction()

function(add_lotus_gsa_test_suite test_name)
    add_executable(${test_name} ${ARGN})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_link_libraries(${test_name}
        lotus_test_utils
        CanaryGSA)

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_gvfg_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryGuardedValueFlow
            ${Z3_LIBRARIES})
endfunction()

function(add_lotus_gvfg_test_suite test_name)
    add_executable(${test_name} ${ARGN})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_link_libraries(${test_name}
        lotus_test_utils
        CanaryGuardedValueFlow
        ${Z3_LIBRARIES})

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_pointer_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryAliasAnalysisWrapper
            LotusAA)
endfunction()

function(add_lotus_alloc_aa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryAllocAA)
endfunction()

function(add_lotus_aser_pta_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            AserPTA)
endfunction()

function(add_lotus_dda_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            DDA)
endfunction()

function(add_lotus_dyck_aa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryDyckAA)
endfunction()

function(add_lotus_sparrow_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            Andersen)
endfunction()

function(add_lotus_tpa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            TPATransforms
            FSCSPointerAnalysis)
endfunction()

function(add_lotus_underapprox_aa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryUnderApproxAA)
endfunction()

function(add_lotus_dfpa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            AserPTA
            CanaryDFPA)
endfunction()

function(add_lotus_solver_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            SymAbs
            ${Z3_LIBRARIES})
endfunction()

function(add_lotus_typehierarchy_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryTypeHirarchy)
endfunction()

function(add_lotus_utils_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            CanaryADT
            CanaryLLVMUtils
            CanaryFormats
            CanaryParallel
            CanarySupport)
endfunction()

function(add_lotus_verification_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            SifaAnalysis
            CanaryAliasAnalysisWrapper
            ${Z3_LIBRARIES})
endfunction()

function(add_lotus_failure_directed_trimming_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            FailureDirectedTrimming
            CanaryAliasAnalysisWrapper
            ${Z3_LIBRARIES})
endfunction()

function(add_lotus_symabs_verification_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            SifaAnalysis
            SymAbsAIAnalysis
            CanaryAliasAnalysisWrapper
            ${Z3_LIBRARIES})
endfunction()

function(add_lotus_backend_verification_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            VerificationBackend)
endfunction()

function(add_lotus_ifdside_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            IFDS
            CanaryAliasAnalysisWrapper)
endfunction()

function(add_lotus_ifdside_test_suite test_name)
    add_executable(${test_name} ${ARGN})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_compile_definitions(${test_name} PRIVATE
        LOTUS_GTEST_NO_MAIN)
    target_link_libraries(${test_name}
        lotus_test_utils
        IFDS
        CanaryAliasAnalysisWrapper)

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

function(add_lotus_mono_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            MONODataFlow
            CanaryAliasAnalysisWrapper)
endfunction()

function(add_lotus_npa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            NPADataFlow
            CanaryAliasAnalysisWrapper
            CanaryParallel)
endfunction()

function(add_lotus_apa_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            APADataFlow)
endfunction()

function(add_lotus_wpds_test test_name source_file)
    add_lotus_targeted_test(${test_name} ${source_file}
        LINK_LIBS
            WPDSDataFlow
            wpds++)
endfunction()

function(add_lotus_pdg_test test_name source_file)
    if(IS_ABSOLUTE ${source_file})
        set(source ${source_file})
    else()
        set(source ${CMAKE_CURRENT_SOURCE_DIR}/${source_file})
    endif()

    add_executable(${test_name} ${source})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
    target_link_libraries(${test_name}
        lotus_test_utils
        LLVMTransformUtils)

    add_test(NAME ${test_name} COMMAND ${LOTUS_TEST_BIN_DIR}/${test_name})
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endfunction()

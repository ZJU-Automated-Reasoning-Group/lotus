set(LOTUS_UNIT_TEST_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(LOTUS_UNIT_TEST_COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/../utils
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../include
    ${LLVM_INCLUDE_DIRS})

add_library(lotus_test_utils INTERFACE)
target_include_directories(lotus_test_utils INTERFACE
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_utils INTERFACE
    GTest::gtest
    GTest::gtest_main
    LLVMAsmParser
    LLVMIRReader
    LLVMPasses)

add_library(lotus_test_harness_utils INTERFACE)
target_include_directories(lotus_test_harness_utils INTERFACE
    ${LOTUS_UNIT_TEST_COMMON_INCLUDES})
target_link_libraries(lotus_test_harness_utils INTERFACE
    GTest::gtest
    LLVMAsmParser
    LLVMIRReader
    LLVMPasses)

include(GoogleTest)

# Child directories declare sources; a parent links one cohesive executable.
function(lotus_collect_test_sources suite_name)
    foreach(source_file IN LISTS ARGN)
        if(IS_ABSOLUTE "${source_file}")
            set(source "${source_file}")
        else()
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
        endif()
        set_property(GLOBAL APPEND PROPERTY
            LOTUS_TEST_SUITE_${suite_name}_SOURCES "${source}")
    endforeach()
endfunction()

function(add_lotus_collected_test_suite suite_name)
    get_property(sources GLOBAL PROPERTY
        LOTUS_TEST_SUITE_${suite_name}_SOURCES)
    if(NOT sources)
        message(FATAL_ERROR
            "add_lotus_collected_test_suite(${suite_name}) has no sources")
    endif()
    add_lotus_test_suite(${suite_name} SOURCES ${sources} ${ARGN})
endfunction()

function(add_lotus_test_suite test_name)
    cmake_parse_arguments(LOTUS_SUITE "" "TIMEOUT;TEST_KIND"
        "SOURCES;LINK_LIBS;INCLUDE_DIRS;COMPILE_DEFINITIONS;DEPENDS;LABELS"
        ${ARGN})
    if(NOT LOTUS_SUITE_SOURCES)
        message(FATAL_ERROR
            "add_lotus_test_suite(${test_name}) requires at least one source")
    endif()

    add_executable(${test_name} ${LOTUS_SUITE_SOURCES})
    set_target_properties(${test_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${LOTUS_TEST_BIN_DIR}
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(${test_name} PRIVATE
        ${LOTUS_UNIT_TEST_COMMON_INCLUDES}
        ${LOTUS_SUITE_INCLUDE_DIRS})
    target_compile_definitions(${test_name} PRIVATE
        LOTUS_GTEST_NO_MAIN
        ${LOTUS_SUITE_COMPILE_DEFINITIONS})
    target_link_libraries(${test_name}
        lotus_test_utils
        ${LOTUS_SUITE_LINK_LIBS})
    if(LOTUS_SUITE_DEPENDS)
        add_dependencies(${test_name} ${LOTUS_SUITE_DEPENDS})
    endif()

    if(LOTUS_SUITE_TIMEOUT)
        set(test_timeout ${LOTUS_SUITE_TIMEOUT})
    else()
        set(test_timeout ${LOTUS_UNIT_TEST_TIMEOUT})
    endif()
    if(LOTUS_SUITE_TEST_KIND)
        string(TOLOWER "${LOTUS_SUITE_TEST_KIND}" test_kind)
    else()
        set(test_kind unit)
    endif()
    if(NOT test_kind MATCHES "^(unit|component|integration)$")
        message(FATAL_ERROR
            "add_lotus_test_suite(${test_name}) has invalid TEST_KIND "
            "'${LOTUS_SUITE_TEST_KIND}'")
    endif()

    file(RELATIVE_PATH relative_dir
        ${LOTUS_UNIT_TEST_ROOT} ${CMAKE_CURRENT_SOURCE_DIR})
    if(relative_dir STREQUAL "")
        set(subsystem unit)
    else()
        string(REGEX REPLACE "/.*$" "" subsystem "${relative_dir}")
        string(TOLOWER "${subsystem}" subsystem)
    endif()

    set(labels lotus ${test_kind} ${subsystem} ${LOTUS_SUITE_LABELS})
    list(REMOVE_DUPLICATES labels)
    string(REPLACE ";" "\\;" labels_property "${labels}")
    gtest_discover_tests(${test_name}
        TEST_PREFIX "${test_name}."
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        PROPERTIES
            TIMEOUT ${test_timeout}
            LABELS "${labels_property}")
endfunction()

function(add_lotus_targeted_test test_name source_file)
    cmake_parse_arguments(LOTUS_TEST "" "" "LINK_LIBS;INCLUDE_DIRS" ${ARGN})
    if(IS_ABSOLUTE "${source_file}")
        set(source "${source_file}")
    else()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
    endif()
    add_lotus_test_suite(${test_name}
        SOURCES ${source}
        LINK_LIBS ${LOTUS_TEST_LINK_LIBS}
        INCLUDE_DIRS ${LOTUS_TEST_INCLUDE_DIRS})
endfunction()

function(add_lotus_concurrency_test_suite test_name)
    add_lotus_test_suite(${test_name}
        SOURCES ${ARGN}
        LINK_LIBS Concurrency CanaryParallel)
endfunction()

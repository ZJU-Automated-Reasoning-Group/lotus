# TODO: Following the code of clam to add the tests in tests/clam

cmake_minimum_required(VERSION 3.10)
find_package(LLVM REQUIRED CONFIG)
# GTest should already be found or downloaded by FindGTest.cmake in parent CMakeLists.txt
# Try find_package, but if targets already exist (from add_subdirectory), that's fine too

find_package(GTest QUIET)
if(NOT TARGET GTest::gtest AND NOT TARGET gtest)
    message(FATAL_ERROR "GTest not found. This should have been handled by FindGTest.cmake")
endif()

add_definitions(${LLVM_DEFINITIONS})
add_definitions(-DCMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set(LOTUS_UNIT_TEST_TIMEOUT 60 CACHE STRING
    "Default timeout in seconds for Lotus unit tests registered with CTest")

find_program(LOTUS_PYTHON_EXECUTABLE NAMES python3 python REQUIRED)
set(LOTUS_UNIT_REGISTRATION_CHECK
    ${CMAKE_SOURCE_DIR}/scripts/check_unit_test_registration.py)
execute_process(
    COMMAND ${LOTUS_PYTHON_EXECUTABLE} ${LOTUS_UNIT_REGISTRATION_CHECK}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit
    RESULT_VARIABLE LOTUS_UNIT_REGISTRATION_RESULT
    OUTPUT_VARIABLE LOTUS_UNIT_REGISTRATION_OUTPUT
    ERROR_VARIABLE LOTUS_UNIT_REGISTRATION_ERROR)
if(NOT LOTUS_UNIT_REGISTRATION_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Unit-test source registration check failed:\n"
        "${LOTUS_UNIT_REGISTRATION_OUTPUT}${LOTUS_UNIT_REGISTRATION_ERROR}")
endif()

add_test(NAME unit_test_registration_check
    COMMAND ${LOTUS_PYTHON_EXECUTABLE} ${LOTUS_UNIT_REGISTRATION_CHECK}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit)
set_tests_properties(unit_test_registration_check PROPERTIES
    LABELS "lotus;infrastructure;unit"
    TIMEOUT 30)

# Add unit test subdirectory
add_subdirectory(unit)

# Note: Individual unit tests are registered in their respective subdirectories under unit/

enable_testing()

# Custom target to run all tests
add_custom_target(run_all_tests 
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom target to run only unit tests
add_custom_target(run_unit_tests 
    COMMAND ${CMAKE_CTEST_COMMAND} -L unit --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_custom_target(run_component_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L component --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L integration --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(LOTUS_ENABLE_COVERAGE)
    find_program(LOTUS_LLVM_PROFDATA
        NAMES llvm-profdata llvm-profdata-14
        HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR}
        REQUIRED)
    find_program(LOTUS_LLVM_COV
        NAMES llvm-cov llvm-cov-14
        HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR}
        REQUIRED)
    add_custom_target(coverage_report
        COMMAND ${LOTUS_PYTHON_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/generate_llvm_coverage.py
            --build-dir ${CMAKE_BINARY_DIR}
            --source-dir ${CMAKE_SOURCE_DIR}
            --ctest ${CMAKE_CTEST_COMMAND}
            --profdata ${LOTUS_LLVM_PROFDATA}
            --cov ${LOTUS_LLVM_COV}
            --label unit
            --minimum ${LOTUS_COVERAGE_MINIMUM}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL)
endif()

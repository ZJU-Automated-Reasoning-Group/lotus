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
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

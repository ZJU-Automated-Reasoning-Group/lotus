cmake_minimum_required(VERSION 3.10)
project(lotus C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set C++14 standard and compiler flags
set(CMAKE_CXX_STANDARD 14)
# set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -frtti")
# -fno-rtti #we need RTTI.

if(APPLE)
  # BSD `ar` appends duplicate members when invoked with `q` against an
  # existing archive. Use `r` so repeated static-library link steps replace
  # members instead of corrupting the archive with duplicate objects.
  set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> cr <TARGET> <LINK_FLAGS> <OBJECTS>")
  set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> cr <TARGET> <LINK_FLAGS> <OBJECTS>")
endif()

# Set output directories
# Libraries and archives go to lib/
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# Default runtime output (tools) goes to bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Test binaries go to bin/tests/
set(LOTUS_TEST_BIN_DIR ${CMAKE_BINARY_DIR}/bin/tests CACHE PATH "Directory for test binaries")
# Example binaries go to bin/examples/
set(LOTUS_EXAMPLE_BIN_DIR ${CMAKE_BINARY_DIR}/bin/examples CACHE PATH "Directory for example binaries")

# Find and configure LLVM (factored out for clarity)
include(cmake/ConfigureLLVM.cmake)

# Include dependency configuration modules
include(cmake/FindZ3.cmake)

# Configure module options
option(ENABLE_CLAM "Enable CLAM abstract interpretation framework" ON)
option(ENABLE_SEAHORN "Enable SeaHorn" ON)
option(LOTUS_USE_CCLYZER "Enable optional cclyzer++ alias analysis backend" OFF)
option(ENABLE_TYPE_QUALIFIER
       "Enable the opt-in TypeQualifier uninitialized-data checker" OFF)

# Configure Boost only if modules that require it are enabled
# Sea-DSA no longer requires Boost, so Boost is only needed for:
# - SeaHorn (ENABLE_SEAHORN)
# - CLAM (ENABLE_CLAM)
# - CclyzerAA (LOTUS_USE_CCLYZER)
if(ENABLE_CLAM OR ENABLE_SEAHORN OR LOTUS_USE_CCLYZER)
    include(cmake/ConfigureBoost.cmake)
endif()

option(DOWNLOAD_CRAB "Download and build CRAB if not found" ON)
set(CUSTOM_CRAB_ROOT "" CACHE PATH "Path to custom CRAB installation (optional)")
if (ENABLE_CLAM)
    include(cmake/ConfigureClamCrab.cmake)
endif()

option(ENABLE_SVF "Enable SVF" OFF)
if (ENABLE_SVF)
    include(cmake/ConfigureSVF.cmake)
endif()

if (ENABLE_SEAHORN)
    include(cmake/ConfigureSeaHorn.cmake)
endif()

# Optional cclyzer++ Datalog-based alias analysis (standalone backend, not in AliasAnalysisWrapper)
# Note: CclyzerAA requires Boost, so Boost will be configured if this is enabled
if (LOTUS_USE_CCLYZER)
    include(cmake/ConfigureCclyzerpp.cmake)
endif()

# Third-party libraries (CUDD, WPDS, spdlog)
add_subdirectory(third-party)

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(benchmarks)

option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Add tests if enabled
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    include(cmake/FindGTest.cmake)
    enable_testing()
    add_subdirectory(tests)
endif()

# Add Python scripts and utilities
add_subdirectory(scripts)

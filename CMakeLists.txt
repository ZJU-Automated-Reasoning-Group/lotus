cmake_minimum_required(VERSION 3.10)
project(lotus C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD 17)
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

# Configure Lotus-owned options in one place.
include(cmake/LotusOptions.cmake)

if(LOTUS_ENABLE_COVERAGE)
    if(NOT LOTUS_BUILD_TESTS)
        message(FATAL_ERROR
            "LOTUS_ENABLE_COVERAGE requires LOTUS_BUILD_TESTS=ON")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
        message(FATAL_ERROR
            "LOTUS_ENABLE_COVERAGE requires Clang or AppleClang")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate")
endif()

# Configure Boost only if modules that require it are enabled
# Sea-DSA no longer requires Boost, so Boost is only needed for:
# - SeaHorn (LOTUS_ENABLE_SEAHORN)
# - CLAM (LOTUS_ENABLE_CLAM)
# - CclyzerAA (LOTUS_USE_CCLYZER)
if(LOTUS_ENABLE_CLAM OR LOTUS_ENABLE_SEAHORN OR LOTUS_ENABLE_SMACK OR
   LOTUS_USE_CCLYZER)
    include(cmake/ConfigureBoost.cmake)
endif()

if(LOTUS_ENABLE_CLAM)
    include(cmake/ConfigureClamCrab.cmake)
endif()

if(LOTUS_ENABLE_SVF)
    include(cmake/ConfigureSVF.cmake)
endif()

if(LOTUS_ENABLE_SEAHORN)
    include(cmake/ConfigureSeaHorn.cmake)
endif()

# Optional cclyzer++ Datalog-based alias analysis (standalone backend, not in AliasAnalysisWrapper)
# Note: CclyzerAA requires Boost, so Boost will be configured if this is enabled
if (LOTUS_USE_CCLYZER)
    include(cmake/ConfigureCclyzerpp.cmake)
endif()

if(LOTUS_BUILD_TESTS)
    enable_testing()
endif()

# Third-party libraries (CUDD, WPDS, spdlog)
add_subdirectory(third-party)

# Add subdirectories
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(benchmarks)

if(LOTUS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Add tests if enabled
if(LOTUS_BUILD_TESTS)
    include(cmake/FindGTest.cmake)
    add_subdirectory(tests)
endif()

# Add Python scripts and utilities
add_subdirectory(scripts)

install(FILES
  config/thread.spec
  config/concurrency_api.spec
  config/openmp_api.spec
  config/mpi_api.spec
  config/linux_kernel_api.spec
  DESTINATION share/lotus/config)

lotus_print_build_summary()

# CLAM and CRAB configuration

if(LOTUS_ENABLE_CLAM)
    set(LOTUS_VENDORED_CRAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third-party/crab")
    set(LOTUS_CLAM_INCLUDE_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/third-party/verification/clam/include")

    # Determine CRAB root directory
    if(LOTUS_CUSTOM_CRAB_ROOT)
        set(CRAB_ROOT ${LOTUS_CUSTOM_CRAB_ROOT})
        message(STATUS "Using custom CRAB at: ${CRAB_ROOT}")
    elseif(EXISTS "${LOTUS_VENDORED_CRAB_ROOT}/CMakeLists.txt")
        set(CRAB_ROOT "${LOTUS_VENDORED_CRAB_ROOT}")
        message(STATUS "Using vendored CRAB from third-party: ${CRAB_ROOT}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crab/CMakeLists.txt")
        # Backward-compatible fallback for older checkouts.
        set(CRAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/crab")
        message(WARNING
            "Using legacy CRAB source tree at ${CRAB_ROOT}. "
            "Please move it to third-party/crab.")
    elseif(LOTUS_DOWNLOAD_CRAB)
        # Download CRAB to build/deps at configure time
        set(CRAB_ROOT "${CMAKE_BINARY_DIR}/deps/crab")
        if(NOT EXISTS "${CRAB_ROOT}/CMakeLists.txt")
            find_package(Git REQUIRED)
            message(STATUS "CRAB not found, downloading to ${CRAB_ROOT}...")
            
            # Clone CRAB at configure time
            execute_process(
                COMMAND ${GIT_EXECUTABLE} clone https://github.com/seahorn/crab.git ${CRAB_ROOT}
                RESULT_VARIABLE GIT_RESULT
                OUTPUT_VARIABLE GIT_OUTPUT
                ERROR_VARIABLE GIT_ERROR
            )
            
            if(NOT GIT_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to clone CRAB: ${GIT_ERROR}")
            endif()
            
            if(NOT EXISTS "${CRAB_ROOT}/CMakeLists.txt")
                message(FATAL_ERROR "CRAB clone succeeded but CMakeLists.txt not found")
            endif()
            
            # Patch CRAB's CMakeLists.txt to update cmake_minimum_required for newer CMake versions
            file(READ "${CRAB_ROOT}/CMakeLists.txt" CRAB_CMAKE_CONTENT)
            # Match cmake_minimum_required with various formats (e.g., VERSION 2.8, 2.8.12, with FATAL_ERROR, etc.)
            string(REGEX REPLACE "cmake_minimum_required\\(VERSION [0-9]+\\.[0-9]+[^)]*\\)" 
                                 "cmake_minimum_required(VERSION 3.10)" 
                                 CRAB_CMAKE_CONTENT "${CRAB_CMAKE_CONTENT}")
            file(WRITE "${CRAB_ROOT}/CMakeLists.txt" "${CRAB_CMAKE_CONTENT}")
            message(STATUS "Patched CRAB CMakeLists.txt to require CMake 3.10+")
            
            message(STATUS "CRAB successfully downloaded to ${CRAB_ROOT}")
        else()
            message(STATUS "Using previously downloaded CRAB at: ${CRAB_ROOT}")
        endif()
    else()
        message(WARNING 
            "CRAB not found. Either:\n"
            "  1. Set -DLOTUS_CUSTOM_CRAB_ROOT=/path/to/crab, or\n"
            "  2. Vendor CRAB into third-party/crab, or\n"
            "  3. Enable auto-download with -DLOTUS_DOWNLOAD_CRAB=ON\n"
            "CLAM will be disabled.")
        set(LOTUS_ENABLE_CLAM OFF)
    endif()
    
    # Configure CRAB if found
    if(LOTUS_ENABLE_CLAM AND EXISTS "${CRAB_ROOT}/CMakeLists.txt")
        message(STATUS "Configuring CRAB at: ${CRAB_ROOT}")
        
        # Set CRAB options before including it
        set(CRAB_BUILD_LIBS_SHARED OFF)
        set(CRAB_USE_LDD OFF)
        set(CRAB_USE_APRON OFF)
        set(CRAB_USE_ELINA OFF)
        
        # Add CRAB as subdirectory
        add_subdirectory(${CRAB_ROOT} ${CMAKE_BINARY_DIR}/crab)

        # CRAB's Lotus-specific NTT implementation includes the CLAM
        # compatibility API as `clam/...`.
        target_include_directories(Crab PRIVATE
            ${LOTUS_CLAM_INCLUDE_ROOT}/Verification
            ${CMAKE_BINARY_DIR}/include/Verification)
        
        # Configure CLAM config.h
        set(HAVE_LLVM_SEAHORN FALSE)
        set(USE_DBM_BIGNUM FALSE)
        set(USE_DBM_SAFEINT FALSE)
        set(INCLUDE_ALL_DOMAINS TRUE)
        set(CLAM_IS_TOPLEVEL FALSE)
        
        configure_file(
            ${LOTUS_CLAM_INCLUDE_ROOT}/Verification/clam/config.h.cmake
            ${CMAKE_BINARY_DIR}/include/Verification/clam/config.h
        )

        add_library(LotusClamBackendConfig INTERFACE)
        add_library(Lotus::ClamBackendConfig ALIAS LotusClamBackendConfig)
        target_include_directories(LotusClamBackendConfig INTERFACE
            ${LOTUS_CLAM_INCLUDE_ROOT}
            ${LOTUS_CLAM_INCLUDE_ROOT}/Verification
            ${LOTUS_CLAM_INCLUDE_ROOT}/Verification/clam
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_BINARY_DIR}/include/Verification)
        target_include_directories(LotusClamBackendConfig SYSTEM INTERFACE
            ${CRAB_ROOT}/include)
        target_compile_definitions(LotusClamBackendConfig INTERFACE HAVE_CLAM=1)

        set(HAVE_CLAM 1)
        
        # Set CLAM libraries - must include both ClamAnalysis and Crab
        set(CLAM_LIBS ClamAnalysis Crab)
        
        message(STATUS "CLAM integration enabled with CRAB")
    endif()
endif()

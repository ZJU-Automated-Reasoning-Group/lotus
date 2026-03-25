# Handle Boost dependencies for SeaHorn, CLAM, and CclyzerAA
# This file is only included when ENABLE_CLAM, ENABLE_SEAHORN, or LOTUS_USE_CCLYZER is ON.
# If all three are disabled, Boost is not configured.
#
# Modules that require Boost:
#   - SeaHorn: Expr, HornClauseDB, BoogieWriter, graph traits, etc.
#   - CLAM: CfgBuilder (hash), read_json (Boost.JSON >= 1.80), Optimizer (optional)
#   - CclyzerAA: flyweight
# Sea-DSA no longer requires Boost.

option(DOWNLOAD_BOOST "Download and build Boost if not found" ON)
set(CUSTOM_BOOST_ROOT "" CACHE PATH "Path to custom boost installation.")

if(CUSTOM_BOOST_ROOT)
    set(BOOST_ROOT ${CUSTOM_BOOST_ROOT})
    set(Boost_NO_SYSTEM_PATHS "ON")
endif()

# Try to find Boost - note that system is header-only in Boost 1.69+
find_package(Boost 1.65 COMPONENTS thread)

# Initialize availability flags
set(BOOST_SYSTEM_AVAILABLE OFF)
set(BOOST_THREAD_AVAILABLE OFF)

# Check if Boost was found with required components
if(Boost_FOUND)
    # In Boost 1.69+, system is header-only, so we just check if Boost headers are available
    # If Boost is found, system headers are available
    set(BOOST_SYSTEM_AVAILABLE ON)
    
    # Thread must have a compiled library component
    if(TARGET Boost::thread OR Boost_THREAD_FOUND)
        set(BOOST_THREAD_AVAILABLE ON)
    else()
        set(BOOST_THREAD_AVAILABLE OFF)
    endif()
endif()

if((NOT Boost_FOUND OR NOT BOOST_SYSTEM_AVAILABLE OR NOT BOOST_THREAD_AVAILABLE) AND DOWNLOAD_BOOST)
    include(ExternalProject)
    set(BOOST_INSTALL_DIR ${CMAKE_BINARY_DIR}/deps/boost)
    message(STATUS "Boost not found, will download and build it in ${BOOST_INSTALL_DIR}")

    ExternalProject_Add(boost
        GIT_REPOSITORY https://github.com/boostorg/boost.git
        GIT_TAG boost-1.65.0
        PREFIX ${CMAKE_BINARY_DIR}/deps
        CONFIGURE_COMMAND ./bootstrap.sh --prefix=${BOOST_INSTALL_DIR}
        BUILD_COMMAND ./b2 --with-system --with-thread install
        BUILD_IN_SOURCE 1
        INSTALL_COMMAND ""
        INSTALL_DIR ${BOOST_INSTALL_DIR}
    )

    set(BOOST_ROOT ${BOOST_INSTALL_DIR})
    set(BOOST_INCLUDEDIR ${BOOST_INSTALL_DIR}/include)
    include_directories(${BOOST_INCLUDEDIR})
    set(BOOST_SYSTEM_AVAILABLE ON)
    set(BOOST_THREAD_AVAILABLE ON)
    set(BOOST_EXTERNAL_PROJECT ON)
    # Set Boost_SYSTEM_LIBRARY for SeaHorn tools (system is header-only in Boost 1.69+)
    set(Boost_SYSTEM_LIBRARY "")
elseif(Boost_FOUND)
    message(STATUS "Found Boost: ${Boost_INCLUDE_DIRS}")
    include_directories(${Boost_INCLUDE_DIRS})
    # Ensure Boost_SYSTEM_LIBRARY is set for SeaHorn tools that reference it
    if(TARGET Boost::system)
        set(Boost_SYSTEM_LIBRARY Boost::system)
    elseif(Boost_SYSTEM_LIBRARY)
        # Already set by find_package
    else()
        # System is header-only in Boost 1.69+, so no library needed
        set(Boost_SYSTEM_LIBRARY "")
    endif()
    if(NOT LLVM_ENABLE_EH)
        add_definitions(-DBOOST_NO_EXCEPTIONS)
    endif()
else()
    message(FATAL_ERROR "Boost is required for SeaHorn, CLAM, and/or CclyzerAA but was not found. "
                        "Set DOWNLOAD_BOOST=ON to auto-download, or provide CUSTOM_BOOST_ROOT.")
endif()


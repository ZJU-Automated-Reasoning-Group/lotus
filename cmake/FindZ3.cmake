# Find Z3 from an explicit prefix, pkg-config, or normal system paths.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_Z3 QUIET z3)
endif()

set(_Z3_HINTS
  ${Z3_ROOT}
  ${Z3_DIR}
  $ENV{Z3_ROOT}
  $ENV{Z3_DIR}
)

find_path(Z3_INCLUDE_DIR
  NAMES z3++.h
  HINTS ${_Z3_HINTS} ${PC_Z3_INCLUDE_DIRS}
  PATH_SUFFIXES include z3)

find_library(Z3_LIBRARY
  NAMES z3
  HINTS ${_Z3_HINTS} ${PC_Z3_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64)

# Z3 >= 4.8.1 exposes its version in z3_version.h.
if(Z3_INCLUDE_DIR AND EXISTS "${Z3_INCLUDE_DIR}/z3_version.h")
  file(READ "${Z3_INCLUDE_DIR}/z3_version.h" _z3_version_h)
  string(REGEX MATCH "Z3_MAJOR_VERSION[ \t]+([0-9]+)" _ "${_z3_version_h}")
  set(_z3_major "${CMAKE_MATCH_1}")
  string(REGEX MATCH "Z3_MINOR_VERSION[ \t]+([0-9]+)" _ "${_z3_version_h}")
  set(_z3_minor "${CMAKE_MATCH_1}")
  string(REGEX MATCH "Z3_BUILD_NUMBER[ \t]+([0-9]+)" _ "${_z3_version_h}")
  set(_z3_build "${CMAKE_MATCH_1}")
  if(_z3_major AND _z3_minor AND _z3_build)
    set(Z3_VERSION_STRING "${_z3_major}.${_z3_minor}.${_z3_build}")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Z3
  REQUIRED_VARS Z3_LIBRARY Z3_INCLUDE_DIR
  VERSION_VAR Z3_VERSION_STRING)

set(Z3_LIBRARIES "${Z3_LIBRARY}")
set(Z3_INCLUDES "${Z3_INCLUDE_DIR}")

if(NOT TARGET Z3::z3)
  add_library(Z3::z3 UNKNOWN IMPORTED)
  set_target_properties(Z3::z3 PROPERTIES
    IMPORTED_LOCATION "${Z3_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Z3_INCLUDE_DIR}")
endif()

message(STATUS "Found Z3 ${Z3_VERSION_STRING}: ${Z3_LIBRARY}")
include_directories("${Z3_INCLUDE_DIR}")

mark_as_advanced(Z3_INCLUDE_DIR Z3_LIBRARY)

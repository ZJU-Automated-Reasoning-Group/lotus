# SeaHorn configuration

# Configure GitSHA1.cc
set(GIT_SHA1 "unknown")
find_package(Git)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_SHA1
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
configure_file(
  "${CMAKE_SOURCE_DIR}/third-party/verification/seahorn/src/Support/GitSHA1.cc.in"
  "${CMAKE_BINARY_DIR}/third-party/verification/seahorn/Support/GitSHA1.cc"
  @ONLY
)
set(SEAHORN_GIT_SHA1_SOURCE
    "${CMAKE_BINARY_DIR}/third-party/verification/seahorn/Support/GitSHA1.cc")

# SeaHorn configuration
set(SeaHorn_VERSION_INFO "dev")
set(HAVE_CLAM ${HAVE_CLAM})
set(HAVE_DSA ON)
set(HAVE_LLVM_SEAHORN OFF)
configure_file(
  ${CMAKE_SOURCE_DIR}/third-party/verification/seahorn/include/Verification/seahorn/config.h.cmake
  ${CMAKE_BINARY_DIR}/include/Verification/seahorn/config.h
)

# SeaHorn's bit-vector operational semantics includes the header-only CLAM
# query bridge even in a SeaHorn-only build. Generate the compatibility config
# header without enabling or linking the CLAM backend.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/include/Verification/clam/config.h")
  set(HAVE_LLVM_SEAHORN FALSE)
  set(USE_DBM_BIGNUM FALSE)
  set(USE_DBM_SAFEINT FALSE)
  set(INCLUDE_ALL_DOMAINS TRUE)
  set(CLAM_IS_TOPLEVEL FALSE)
  configure_file(
    ${CMAKE_SOURCE_DIR}/third-party/verification/clam/include/Verification/clam/config.h.cmake
    ${CMAKE_BINARY_DIR}/include/Verification/clam/config.h)
endif()

add_library(LotusSeaHornBackendConfig INTERFACE)
add_library(Lotus::SeaHornBackendConfig ALIAS LotusSeaHornBackendConfig)
target_include_directories(LotusSeaHornBackendConfig INTERFACE
  ${CMAKE_SOURCE_DIR}/third-party/verification/seahorn/include
  ${CMAKE_SOURCE_DIR}/third-party/verification/seahorn/include/Verification
  ${CMAKE_SOURCE_DIR}/third-party/verification/clam/include/Verification
  ${CMAKE_BINARY_DIR}/include
  ${CMAKE_BINARY_DIR}/include/Verification)
target_include_directories(LotusSeaHornBackendConfig SYSTEM INTERFACE
  ${CMAKE_SOURCE_DIR}/third-party/crab/include)

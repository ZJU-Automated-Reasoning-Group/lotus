# Lotus-owned build adapters for vendored verification backends.  Keep this
# directory free of upstream sources: those live under third-party/verification.
set(LOTUS_VERIFICATION_VENDOR_ROOT
    "${CMAKE_SOURCE_DIR}/third-party/verification")

if(LOTUS_ENABLE_CLAM)
  add_subdirectory(
    "${LOTUS_VERIFICATION_VENDOR_ROOT}/clam/src"
    "${CMAKE_CURRENT_BINARY_DIR}/clam")
endif()

if(LOTUS_ENABLE_SEAHORN)
  add_subdirectory(
    "${LOTUS_VERIFICATION_VENDOR_ROOT}/seahorn/src"
    "${CMAKE_CURRENT_BINARY_DIR}/seahorn")
endif()

if(LOTUS_ENABLE_SMACK)
  add_subdirectory(
    "${LOTUS_VERIFICATION_VENDOR_ROOT}/smack/src"
    "${CMAKE_CURRENT_BINARY_DIR}/smack")
endif()

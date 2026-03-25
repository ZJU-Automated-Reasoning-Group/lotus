add_library(VerificationBackend STATIC
  Backend.cpp
)

target_link_libraries(VerificationBackend
  PUBLIC
  LLVMSupport
)

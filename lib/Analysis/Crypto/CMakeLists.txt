set(CryptoVerify_SOURCES
  ctllvm.cpp
  ctllvm_analysis.cpp
  ctllvm_debug_info.cpp
  ctllvm_inlining.cpp
  ctllvm_reporting.cpp
  ctllvm_targets.cpp
)
add_library(CryptoVerify STATIC ${CryptoVerify_SOURCES})

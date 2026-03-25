add_library(CanarySWPrefetching STATIC
  SWPrefetchingAnalysis.cpp
  SWPrefetchingDriver.cpp
  SWPrefetchingMultiPhi.cpp
  SWPrefetchingPass.cpp
  SWPrefetchingSinglePhi.cpp
)
target_link_libraries(CanarySWPrefetching CanaryLLVMUtils)

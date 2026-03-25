llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  Core
  IRReader
  Passes
)

add_executable(lotus-opt lotus-opt.cpp)

set(LOTUS_OPT_COMMON_LIBS
  CanaryMemorySSA
  CanaryLLVMUtils
  SeaDsaAnalysis
  VerificationTransform
  VerificationAnalysis
  ${LLVM_LINK_COMPONENTS}
)

target_link_libraries(lotus-opt PRIVATE ${LOTUS_OPT_COMMON_LIBS})

if(APPLE)
  # On macOS, use -force_load to ensure all symbols are included
  # Explicitly add dependencies to ensure pass libraries are built first
  target_link_libraries(lotus-opt PRIVATE CanaryOptimization CanarySWPrefetching)
  target_link_options(lotus-opt PRIVATE
    -Wl,-force_load,$<TARGET_FILE:CanaryOptimization>
    -Wl,-force_load,$<TARGET_FILE:CanarySWPrefetching>
  )
else()
  # On Linux, use --whole-archive
  target_link_options(lotus-opt PRIVATE
    -Wl,--whole-archive
  )
  target_link_libraries(lotus-opt PRIVATE
    CanaryOptimization
    CanarySWPrefetching
  )
  target_link_options(lotus-opt PRIVATE
    -Wl,--no-whole-archive
  )
endif()

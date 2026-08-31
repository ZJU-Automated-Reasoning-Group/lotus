llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  BitWriter
  IRReader
  Passes
)

set(LOTUS_OPT_COMMON_LIBS
  CanaryPurityAnalysis
  CanaryShadowMemSSA
  CanaryLLVMUtils
  SeaDsaAnalysis
  ${LLVM_LINK_COMPONENTS}
)

set(LOTUS_PREFETCH_COMMON_LIBS
  CanaryLLVMUtils
  ${LLVM_LINK_COMPONENTS}
)

function(configure_force_load target library)
  if(APPLE)
    target_link_libraries(${target} PRIVATE ${library})
    target_link_options(${target} PRIVATE
      -Wl,-force_load,$<TARGET_FILE:${library}>
    )
  else()
    target_link_options(${target} PRIVATE -Wl,--whole-archive)
    target_link_libraries(${target} PRIVATE ${library})
    target_link_options(${target} PRIVATE -Wl,--no-whole-archive)
  endif()
endfunction()

add_executable(lotus-opt-ipo lotus-opt-ipo.cpp)
target_link_libraries(lotus-opt-ipo PRIVATE ${LOTUS_OPT_COMMON_LIBS})
configure_force_load(lotus-opt-ipo CanaryOptimizationIPO)

add_executable(lotus-opt-prefetch lotus-opt-prefetch.cpp)
target_link_libraries(lotus-opt-prefetch PRIVATE ${LOTUS_PREFETCH_COMMON_LIBS})
configure_force_load(lotus-opt-prefetch CanarySWPrefetching)

add_executable(lotus-opt-purity lotus-opt-purity.cpp)
target_link_libraries(lotus-opt-purity PRIVATE ${LOTUS_OPT_COMMON_LIBS})

#add_executable(lotus-lif lotus-opt-lif.cpp)
#target_link_libraries(lotus-lif PRIVATE
#  CanaryLIF
#  LLVMIRReader
#  LLVMPasses
#)

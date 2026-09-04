# Partial Evaluation (LLPE) - upgraded to LLVM 14.x for Lotus.
#
# Implementation directories own their source manifests. CanaryPE remains the
# stable aggregate library consumed by tools and downstream projects.
function(lotus_add_partial_evaluation_sources)
  foreach(source_file IN LISTS ARGN)
    set_property(GLOBAL APPEND PROPERTY LOTUS_PARTIAL_EVALUATION_SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    )
  endforeach()
endfunction()

add_subdirectory(Engine)
add_subdirectory(Commit)
add_subdirectory(Support)

get_property(CanaryPE_SOURCES GLOBAL
  PROPERTY LOTUS_PARTIAL_EVALUATION_SOURCES
)

add_library(CanaryPE STATIC ${CanaryPE_SOURCES})
target_link_libraries(CanaryPE PUBLIC CanaryLLVMUtils)

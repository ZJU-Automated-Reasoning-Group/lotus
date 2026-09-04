# Each implementation directory owns its source manifest. Keep Concurrency as
# the stable aggregate target until the implementation dependencies are acyclic
# enough to expose independently linkable component libraries.
function(lotus_add_concurrency_sources)
  foreach(source_file IN LISTS ARGN)
    set_property(GLOBAL APPEND PROPERTY LOTUS_CONCURRENCY_SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}"
    )
  endforeach()
endfunction()

add_subdirectory(Utils)
add_subdirectory(OpenMP)
add_subdirectory(MHP)
add_subdirectory(LockSet)
add_subdirectory(Memory)
add_subdirectory(Thread)
add_subdirectory(ValueFlow)
add_subdirectory(CUDA)
add_subdirectory(JoinTarget)
add_subdirectory(MPI)
add_subdirectory(LinuxKernel)

get_property(Concurrency_SOURCES GLOBAL PROPERTY LOTUS_CONCURRENCY_SOURCES)
add_library(Concurrency STATIC
  ConcurrencyFacade.cpp
  ${Concurrency_SOURCES}
)

target_link_libraries(Concurrency
  CanaryLLVMUtils
  CanaryAliasAnalysisWrapper
  FlowSensitivePTA
  SVFG
)
target_compile_definitions(Concurrency PRIVATE
  LOTUS_SOURCE_CONFIG_DIR="${PROJECT_SOURCE_DIR}/config"
  LOTUS_INSTALL_CONFIG_DIR="${CMAKE_INSTALL_PREFIX}/share/lotus/config"
)

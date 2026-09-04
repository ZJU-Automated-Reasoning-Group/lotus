add_subdirectory(seahorn)
add_subdirectory(Analysis)
add_subdirectory(Transforms)
add_subdirectory(Support)
add_subdirectory(KInduction)

set(LOTUS_SEAHORN_TARGETS
  seahorn.LIB
  SeaSmt
  SeaAnalysis
  SeaInstrumentation
  SeaTransformsScalar
  SeaTransformsUtils
  SeaTransformsKernel
  SeaSupport
  seahorn.KInduction)

foreach(target_name IN LISTS LOTUS_SEAHORN_TARGETS)
  if(TARGET ${target_name})
    target_link_libraries(${target_name} PUBLIC
      Lotus::SeaHornBackendConfig)
  endif()
endforeach()

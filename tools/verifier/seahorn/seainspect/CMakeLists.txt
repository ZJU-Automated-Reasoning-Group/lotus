set (SEAINSPECT_LIBS
  SeaAnalysis
  SeaInstrumentation
  SeaTransformsScalar
  SeaTransformsUtils 
  ${SEA_DSA_LIBS}
  ${Boost_SYSTEM_LIBRARY}  
  SeaSupport
  ${RT_LIB}
  )

set(LLVM_LINK_COMPONENTS 
  irreader 
  bitwriter 
  ipo 
  scalaropts 
  instrumentation 
  core 
  codegen 
  objcarcopts)


add_executable(seainspect seainspect.cc)
target_compile_definitions(seainspect PRIVATE
  __STDC_CONSTANT_MACROS
  __STDC_LIMIT_MACROS)
if(APPLE)
  set_target_properties(seainspect PROPERTIES LINK_FLAGS "${LINK_FLAGS} -Wl,-search_paths_first")
endif()
target_link_libraries (seainspect PRIVATE ${SEAINSPECT_LIBS})
llvm_config (seainspect ${LLVM_LINK_COMPONENTS})
install(TARGETS seainspect RUNTIME DESTINATION bin)

if (SEAHORN_STATIC_EXE)
  set (CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
  set_target_properties (seainspect PROPERTIES LINK_SEARCH_START_STATIC ON)
  set_target_properties (seainspect PROPERTIES LINK_SEARCH_END_STATIC ON)
endif()

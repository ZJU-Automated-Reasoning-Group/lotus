add_subdirectory(datalog)

if(LOTUS_ENABLE_OWL)
  set(LLVM_LINK_COMPONENTS
      LLVMSupport)

  add_executable(owl owl.cpp)
  target_link_libraries(owl PRIVATE
      CanarySMT
      ${LLVM_LINK_COMPONENTS}
      ${Z3_LIBRARIES})
endif()

if(LOTUS_ENABLE_SMT_STABILIZER)
  add_executable(lotus-smt-stabilizer smt-stabilizer.cpp)
  target_link_libraries(lotus-smt-stabilizer PRIVATE LotusSMTStabilizer)
  set_target_properties(lotus-smt-stabilizer PROPERTIES
      CXX_STANDARD 17
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
endif()

# TODO (mabye do not create independent binaries for slot and staub?)
# slot
# staub

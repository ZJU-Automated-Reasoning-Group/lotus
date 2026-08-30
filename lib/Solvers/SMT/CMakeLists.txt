# SMT-related

add_subdirectory(LIBSMT)
add_subdirectory(SymAbs)
add_subdirectory(SMTSampler)
if(LOTUS_ENABLE_SMT_STABILIZER)
  add_subdirectory(SMTStabilizer)
endif()
# add_subdirectory(STAUB)

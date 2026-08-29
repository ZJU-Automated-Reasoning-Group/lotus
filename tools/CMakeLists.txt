add_subdirectory(alias)
add_subdirectory(checker)
add_subdirectory(dataflow)
add_subdirectory(optimization)
add_subdirectory(solver)
add_subdirectory(verifier)

add_subdirectory(ir)
#add_subdirectory(mcp)        # not used for now

# not used for now
if(LOTUS_ENABLE_CFL)
    add_subdirectory(cfl)
endif()

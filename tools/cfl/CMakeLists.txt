# CFL tools umbrella

add_subdirectory(classical)

if(LOTUS_ENABLE_CSR)
    add_subdirectory(csr)
endif()

add_subdirectory(mcfl)
add_subdirectory(unary-interleaved-dyck)
add_subdirectory(interleaved-dyck-approximation)
add_subdirectory(mutual-refinement)
add_subdirectory(inter-dyck-graph-reduce)

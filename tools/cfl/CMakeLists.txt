# CFL tools umbrella

add_subdirectory(classical)

if(LOTUS_ENABLE_CSR)
    add_subdirectory(csr)
endif()

add_subdirectory(interleaved-dyck)

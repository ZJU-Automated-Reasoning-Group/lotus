add_executable(lotus-cfl-interleaved-dyck-unary
    lotus-cfl-interleaved-dyck-unary.cpp)

target_link_libraries(lotus-cfl-interleaved-dyck-unary
    PRIVATE
        CanaryInterleavedDyckUnary)

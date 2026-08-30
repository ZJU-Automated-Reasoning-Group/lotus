add_executable(lotus-cfl-unary-interleaved-dyck
    lotus-cfl-unary-interleaved-dyck.cpp)

target_link_libraries(lotus-cfl-unary-interleaved-dyck
    PRIVATE
        CanaryUnaryInterleavedDyck)

add_executable(lotus-cfl-interleaved-dyck-approximation
    lotus-cfl-interleaved-dyck-approximation.cpp)

target_link_libraries(lotus-cfl-interleaved-dyck-approximation
    PRIVATE
        CanaryInterleavedDyckApproximation)

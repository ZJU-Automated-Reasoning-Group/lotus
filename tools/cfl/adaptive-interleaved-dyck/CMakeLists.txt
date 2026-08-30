add_executable(lotus-cfl-adaptive-interleaved-dyck
    lotus-cfl-adaptive-interleaved-dyck.cpp)

target_link_libraries(lotus-cfl-adaptive-interleaved-dyck
    PRIVATE
        CanaryAdaptiveInterleavedDyck)

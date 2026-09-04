add_executable(lotus-cfl-interleaved-dyck-staged-bounds
    lotus-cfl-interleaved-dyck-staged-bounds.cpp)

target_link_libraries(lotus-cfl-interleaved-dyck-staged-bounds
    PRIVATE
        CanaryInterleavedDyckStagedBounds)

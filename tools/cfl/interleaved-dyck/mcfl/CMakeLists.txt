add_executable(lotus-cfl-interleaved-dyck-mcfl lotus-cfl-interleaved-dyck-mcfl.cpp)
target_link_libraries(lotus-cfl-interleaved-dyck-mcfl
    PRIVATE
        CanaryInterleavedDyckMCFL)

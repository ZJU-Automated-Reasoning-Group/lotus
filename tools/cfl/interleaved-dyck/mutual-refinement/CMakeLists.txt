add_executable(lotus-cfl-interleaved-dyck-mutual-refinement
    lotus-cfl-interleaved-dyck-mutual-refinement.cpp)

target_link_libraries(lotus-cfl-interleaved-dyck-mutual-refinement
    PRIVATE
        CanaryInterleavedDyckMutualRefinement)

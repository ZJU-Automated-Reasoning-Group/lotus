add_executable(lotus-cfl-mutual-refinement
    lotus-cfl-mutual-refinement.cpp)

target_link_libraries(lotus-cfl-mutual-refinement
    PRIVATE
        MutualRefinement)

add_executable(lotus-cfl-inter-dyck-graphaux
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterDyckGraphReduce/GraphAux.cpp)
target_link_libraries(lotus-cfl-inter-dyck-graphaux
    PRIVATE
        CanaryADT
        CanaryInterDyckGraphReduce)

add_executable(lotus-cfl-inter-dyck-dkmerge
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterDyckGraphReduce/dkMerge.cpp)
target_link_libraries(lotus-cfl-inter-dyck-dkmerge
    PRIVATE
        CanaryInterDyckGraphReduce)

configure_file(
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterDyckGraphReduce/graph_simp.py
    ${CMAKE_BINARY_DIR}/bin/lotus-cfl-inter-dyck-graph-reduce.py
    COPYONLY)

add_custom_target(lotus-cfl-inter-dyck-graph-reduce
    DEPENDS
        lotus-cfl-inter-dyck-graphaux
        lotus-cfl-inter-dyck-dkmerge)

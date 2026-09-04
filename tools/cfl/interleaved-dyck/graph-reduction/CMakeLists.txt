add_executable(lotus-cfl-interleaved-dyck-graphaux
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterleavedDyck/GraphReduction/GraphAux.cpp)
target_link_libraries(lotus-cfl-interleaved-dyck-graphaux
    PRIVATE
        CanaryADT
        CanaryInterleavedDyckGraphReduction)

add_executable(lotus-cfl-interleaved-dyck-dkmerge
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterleavedDyck/GraphReduction/dkMerge.cpp)
target_link_libraries(lotus-cfl-interleaved-dyck-dkmerge
    PRIVATE
        CanaryInterleavedDyckGraphReduction)

configure_file(
    ${CMAKE_SOURCE_DIR}/lib/CFL/InterleavedDyck/GraphReduction/graph_simp.py
    ${CMAKE_BINARY_DIR}/bin/lotus-cfl-interleaved-dyck-graph-reduction.py
    COPYONLY)

add_custom_target(lotus-cfl-interleaved-dyck-graph-reduction
    DEPENDS
        lotus-cfl-interleaved-dyck-graphaux
        lotus-cfl-interleaved-dyck-dkmerge)

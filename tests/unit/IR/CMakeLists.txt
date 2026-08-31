add_subdirectory(PDG)
add_subdirectory(GVFG)
add_subdirectory(MemorySSA)
add_subdirectory(GSA)
add_subdirectory(SVFG)

add_lotus_test_suite(ir_graph_view_tests
  SOURCES
    GraphViewTest.cpp
  LINK_LIBS
    CanaryICFG)

add_custom_target(ir_tests
  DEPENDS
    ir_pdg_tests
    ir_gvfg_tests
    ir_memoryssa_tests
    ir_gsa_tests
    ir_svfg_tests
    ir_graph_view_tests)

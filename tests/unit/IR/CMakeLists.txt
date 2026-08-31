add_subdirectory(PDG)
add_subdirectory(GVFG)
add_subdirectory(MemorySSA)

add_lotus_test_suite(ir_misc_tests
  SOURCES
    GraphViewTest.cpp
    GSATest.cpp
    SVFGSerializerTest.cpp
    SeaDsaFieldTypeTest.cpp
  LINK_LIBS
    CanaryGSA
    CanaryICFG
    SVFG
    CanaryMemorySSA
    SeaDsaAnalysis)

add_custom_target(ir_tests
  DEPENDS
    ir_pdg_tests
    ir_gvfg_tests
    ir_memoryssa_tests
    ir_misc_tests)

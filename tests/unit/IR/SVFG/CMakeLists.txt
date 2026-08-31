add_lotus_test_suite(ir_svfg_tests
  SOURCES
    MemoryRegionPartitionerTest.cpp
    SVFGMemorySSATest.cpp
    SVFGMemorySSAInterproceduralTest.cpp
    SVFGSerializerTest.cpp
  LINK_LIBS
    SVFG)

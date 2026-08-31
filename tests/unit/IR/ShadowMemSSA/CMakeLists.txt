add_lotus_test_suite(ir_shadowmemssa_tests
  SOURCES
    IPOShadowMemSSATest.cpp
  LINK_LIBS
    CanaryShadowMemSSA
    CanaryOptimizationIPO)

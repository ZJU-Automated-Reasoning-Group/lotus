# WPDS DataFlow Unit Tests

add_lotus_test_suite(wpds_test
  SOURCES WPDSTest.cpp
  LINK_LIBS
    WPDSDataFlow
    wpds++)

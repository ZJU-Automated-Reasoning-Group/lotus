add_executable(lotus-datalog-benchmark DatalogBenchmark.cpp)

target_link_libraries(lotus-datalog-benchmark PRIVATE LotusDatalog)

set_target_properties(lotus-datalog-benchmark PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)

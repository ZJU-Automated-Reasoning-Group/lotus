add_executable(lotus-datalog DatalogCli.cpp)

target_link_libraries(lotus-datalog PRIVATE
  LotusDatalogFrontend)

install(TARGETS lotus-datalog RUNTIME DESTINATION bin)

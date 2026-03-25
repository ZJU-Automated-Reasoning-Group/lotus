add_lotus_failure_directed_trimming_test(failure_directed_trimming_test
                                         FailureDirectedTrimmingTest.cpp)
add_lotus_verification_test(sifa_reachability_test SifaReachabilityTest.cpp)
add_lotus_verification_test(sifa_interprocedural_reachability_test SifaInterproceduralReachabilityTest.cpp)
add_lotus_symabs_verification_test(sifa_symabs_smoke_test SifaSymAbsSmokeTest.cpp)
add_lotus_symabs_verification_test(sifa_symabs_llvm_subset_test SifaSymAbsLlvmSubsetTest.cpp)
add_lotus_verification_test(sifa_dag_interpreter_test SifaDagInterpreterTest.cpp)
add_lotus_symabs_verification_test(sifa_verification_test SifaVerificationTest.cpp)

set_tests_properties(sifa_symabs_smoke_test PROPERTIES TIMEOUT 900)
add_lotus_backend_verification_test(backend_test BackendTest.cpp)

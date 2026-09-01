add_lotus_targeted_test(ClassicalCFLTest ClassicalCFLTest.cpp
    LINK_LIBS
        CanaryClassicalCFL)

add_lotus_targeted_test(ClassicalArchitectureTest
    ClassicalArchitectureTest.cpp
    LINK_LIBS
        CanaryClassicalCFL)

add_lotus_targeted_test(scs_index_test SCSIndexTest.cpp
    LINK_LIBS
        CanaryCSIndex)

add_lotus_targeted_test(interleaved_dyck_approximation_test
    InterleavedDyckApproximationTest.cpp
    LINK_LIBS
        CanaryInterleavedDyckApproximation)

add_lotus_test_suite(unary_interleaved_dyck_test
    SOURCES
        UnaryInterleavedDyckAdaptiveTest.cpp
        UnaryInterleavedDyckFixedCounterTest.cpp
    LINK_LIBS
        CanaryUnaryInterleavedDyck)

add_lotus_targeted_test(interleaved_dyck_comparison_test
    InterleavedDyckComparisonTest.cpp
    LINK_LIBS
        CanaryInterleavedDyckApproximation
        CanaryMCFL
        CanaryUnaryInterleavedDyck)
target_compile_definitions(interleaved_dyck_comparison_test PRIVATE
    INTERLEAVED_DYCK_BENCHMARK_DIR="${CMAKE_SOURCE_DIR}/benchmarks/interleaved-dyck-approximation/taint")

add_lotus_targeted_test(mcfl_test MCFLTest.cpp
    LINK_LIBS
        CanaryMCFL)
target_compile_definitions(mcfl_test PRIVATE
    MCFL_BENCHMARK_DIR="${CMAKE_SOURCE_DIR}/benchmarks/interleaved-dyck-approximation/taint")

add_lotus_targeted_test(ClassicalAdaptersTest ClassicalAdaptersTest.cpp
    LINK_LIBS
        CanaryClassicalCFLAlias
        CanaryClassicalCFLSVFG)

foreach(classical_backend baseline pocr hybrid)
    add_test(NAME classical_cfl_cli_${classical_backend}
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/chain.grammar
            --graph ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/chain.txt
            --solver ${classical_backend}
            --json-stats)
    set_tests_properties(classical_cfl_cli_${classical_backend} PROPERTIES
        PASS_REGULAR_EXPRESSION "\"relation_edges\":64")
endforeach()

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

if(TARGET lotus-cfl-classical)
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

    add_test(NAME classical_cfl_cli_infers_attributes
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/call.grammar
            --graph ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/call.txt
            --solver hybrid
            --json-stats)
    set_tests_properties(classical_cfl_cli_infers_attributes PROPERTIES
        PASS_REGULAR_EXPRESSION "\"relation_edges\":3")

    add_test(NAME classical_cfl_cli_hybrid_forest_stats
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/chain.grammar
            --graph ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/chain.txt
            --solver hybrid
            --json-stats)
    set_tests_properties(classical_cfl_cli_hybrid_forest_stats PROPERTIES
        PASS_REGULAR_EXPRESSION "\"hybrid_nodes\":55")

    add_test(NAME classical_cfl_cli_validate_only
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/call.grammar
            --graph ${CMAKE_SOURCE_DIR}/benchmarks/cfl-classical/call.txt
            --validate-only)
    set_tests_properties(classical_cfl_cli_validate_only PROPERTIES
        PASS_REGULAR_EXPRESSION "validation=ok")
endif()

if(TARGET lotus-cfl-alias)
    add_test(NAME classical_cfl_alias_indirect_call
        COMMAND $<TARGET_FILE:lotus-cfl-alias>
            --solver pocr
            --encoding pag
            --json-stats
            ${CMAKE_SOURCE_DIR}/tests/regress/PTATestCases/funptr-simple.ll)
    set_tests_properties(classical_cfl_alias_indirect_call PROPERTIES
        PASS_REGULAR_EXPRESSION "\"callgraph_rounds\":2")

    set(classical_alias_annotation_cases
        funptr-simple
        array-varIdx
        struct-assignment-direct
        heap-indirect
        constraint-cycle-field
        array-varIdx2
        struct-nested-2-layers
        field-ptr-arith-constIdx
        ptr-dereference3
        heap-linkedlist
        global-initializer
        struct-twoflds)
    foreach(alias_case IN LISTS classical_alias_annotation_cases)
        add_test(NAME classical_cfl_alias_annotation_${alias_case}
            COMMAND $<TARGET_FILE:lotus-cfl-alias>
                --solver pocr
                --encoding pag
                --check-annotations
                ${CMAKE_SOURCE_DIR}/tests/regress/PTATestCases/${alias_case}.ll)
        set_tests_properties(classical_cfl_alias_annotation_${alias_case}
            PROPERTIES PASS_REGULAR_EXPRESSION "annotation=pass")
    endforeach()
endif()

get_property(cfl_registered_tests DIRECTORY PROPERTY TESTS)
foreach(cfl_registered_test IN LISTS cfl_registered_tests)
    if(cfl_registered_test MATCHES "^classical_cfl_(cli|alias)_")
        set_tests_properties(${cfl_registered_test} PROPERTIES
            LABELS "lotus;integration;cfl;cli"
            TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    endif()
endforeach()

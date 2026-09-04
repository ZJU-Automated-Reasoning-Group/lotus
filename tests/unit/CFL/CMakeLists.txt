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

add_lotus_targeted_test(mutual_refinement_test
    MutualRefinementTest.cpp
    LINK_LIBS
        MutualRefinement)

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
    INTERLEAVED_DYCK_DATASET_DIR="${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint")

add_lotus_targeted_test(mcfl_test MCFLTest.cpp
    LINK_LIBS
        CanaryMCFL)
target_compile_definitions(mcfl_test PRIVATE
    MCFL_DATASET_DIR="${CMAKE_SOURCE_DIR}/benchmarks/real-world/CFL/InterleavedDyck/taint")

add_lotus_targeted_test(ClassicalAdaptersTest ClassicalAdaptersTest.cpp
    LINK_LIBS
        CanaryClassicalCFLAliasClient
        CanaryClassicalCFLValueFlowClient)

if(TARGET lotus-cfl-classical)
    foreach(classical_backend
            sparse-set sparse-bitvector graspan transitive-closure pocr hpocr
            focr)
        add_test(NAME classical_cfl_cli_${classical_backend}
            COMMAND $<TARGET_FILE:lotus-cfl-classical>
                --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.grammar
                --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.txt
                --solver ${classical_backend}
                --json-stats)
        set_tests_properties(classical_cfl_cli_${classical_backend} PROPERTIES
            PASS_REGULAR_EXPRESSION "\"relation_edges\":64")

        add_test(NAME classical_cfl_pocr_format_${classical_backend}
            COMMAND $<TARGET_FILE:lotus-cfl-classical>
                --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.grammar
                --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
                --solver ${classical_backend}
                --unidirectional
                --json-stats)
        set_tests_properties(
            classical_cfl_pocr_format_${classical_backend} PROPERTIES
            PASS_REGULAR_EXPRESSION "\"relation_edges\":8")
    endforeach()

    add_test(NAME classical_cfl_cli_infers_attributes
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.txt
            --solver transitive-closure
            --json-stats)
    set_tests_properties(classical_cfl_cli_infers_attributes PROPERTIES
        PASS_REGULAR_EXPRESSION "\"relation_edges\":3")

    add_test(NAME classical_cfl_cli_transitive_stats
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/chain.txt
            --solver transitive-closure
            --json-stats)
    set_tests_properties(classical_cfl_cli_transitive_stats PROPERTIES
        PASS_REGULAR_EXPRESSION "\"transitive_instances\":1")

    add_test(NAME classical_cfl_cli_validate_only
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/call.txt
            --validate-only)
    set_tests_properties(classical_cfl_cli_validate_only PROPERTIES
        PASS_REGULAR_EXPRESSION "validation=ok")

    add_test(NAME classical_cfl_cli_graph_simplification
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
            --solver pocr
            --simplification-flavor alias
            --simplify-graph
            --json-stats)
    set_tests_properties(classical_cfl_cli_graph_simplification PROPERTIES
        PASS_REGULAR_EXPRESSION "\"simplified_nodes\":1")

    add_test(NAME classical_cfl_cli_count_symbols
        COMMAND $<TARGET_FILE:lotus-cfl-classical>
            --grammar ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.grammar
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
            --solver pocr
            --unidirectional
            --json-stats)
    set_tests_properties(classical_cfl_cli_count_symbols PROPERTIES
        PASS_REGULAR_EXPRESSION "\"count_edges\":3")

    foreach(pocr_client aa vf)
        if(pocr_client STREQUAL "aa")
            set(expected_rewritten_start_edges 8)
        else()
            set(expected_rewritten_start_edges 6)
        endif()
        foreach(grammar_variant "" "-rewritten")
            foreach(epoch_backend sparse-bitvector graspan)
                string(REPLACE "-" "_" variant_name "${grammar_variant}")
                string(REPLACE "-" "_" backend_name "${epoch_backend}")
                add_test(
                    NAME classical_cfl_grammar_${pocr_client}${variant_name}_${backend_name}
                    COMMAND $<TARGET_FILE:lotus-cfl-classical>
                        --grammar
                        ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr-${pocr_client}${grammar_variant}.grammar
                        --graph
                        ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr-${pocr_client}.graph
                        --solver ${epoch_backend}
                        --json-stats)
                set_tests_properties(
                    classical_cfl_grammar_${pocr_client}${variant_name}_${backend_name}
                    PROPERTIES PASS_REGULAR_EXPRESSION
                    "\"start_edges\":${expected_rewritten_start_edges}")
            endforeach()
        endforeach()
    endforeach()
endif()

if(TARGET lotus-cfl-alias)
    add_test(NAME classical_cfl_alias_indirect_call
        COMMAND $<TARGET_FILE:lotus-cfl-alias>
            --solver sparse-bitvector
            --encoding pag
            --json-stats
            ${CMAKE_SOURCE_DIR}/tests/regress/Alias/PTA/funptr-simple.ll)
    set_tests_properties(classical_cfl_alias_indirect_call PROPERTIES
        PASS_REGULAR_EXPRESSION "\"callgraph_rounds\":2")

    foreach(specialized_alias_engine pocr-aa focr-aa)
        add_test(NAME classical_cfl_alias_${specialized_alias_engine}
            COMMAND $<TARGET_FILE:lotus-cfl-alias>
                --engine ${specialized_alias_engine}
                --encoding peg
                --check-annotations
                --json-stats
                ${CMAKE_SOURCE_DIR}/tests/regress/Alias/PTA/funptr-simple.ll)
        set_tests_properties(
            classical_cfl_alias_${specialized_alias_engine} PROPERTIES
            PASS_REGULAR_EXPRESSION "annotation=pass")
    endforeach()

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
                --solver sparse-bitvector
                --encoding pag
                --check-annotations
                ${CMAKE_SOURCE_DIR}/tests/regress/Alias/PTA/${alias_case}.ll)
        set_tests_properties(classical_cfl_alias_annotation_${alias_case}
            PROPERTIES PASS_REGULAR_EXPRESSION "annotation=pass")
    endforeach()
endif()

if(TARGET lotus-cfl-vf)
    foreach(classical_backend
            sparse-set sparse-bitvector graspan transitive-closure pocr hpocr
            focr)
        add_test(NAME classical_cfl_vf_${classical_backend}
            COMMAND $<TARGET_FILE:lotus-cfl-vf>
                --solver ${classical_backend}
                --query main::seed,main::result
                --json-stats
                ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/value-flow.ll)
        set_tests_properties(classical_cfl_vf_${classical_backend} PROPERTIES
            PASS_REGULAR_EXPRESSION "flow=yes")
    endforeach()
    foreach(specialized_vf_engine pocr-vfa focr-vfa)
        add_test(NAME classical_cfl_vf_${specialized_vf_engine}
            COMMAND $<TARGET_FILE:lotus-cfl-vf>
                --engine ${specialized_vf_engine}
                --query main::seed,main::result
                --json-stats
                ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/value-flow.ll)
        set_tests_properties(classical_cfl_vf_${specialized_vf_engine}
            PROPERTIES PASS_REGULAR_EXPRESSION "flow=yes")
    endforeach()
endif()

if(TARGET lotus-cfl-foldability)
    add_test(NAME classical_cfl_foldability
        COMMAND $<TARGET_FILE:lotus-cfl-foldability>
            --all
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/foldability.rsm
            ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/foldability.patterns)
    set_tests_properties(classical_cfl_foldability PROPERTIES
        PASS_REGULAR_EXPRESSION "foldable.*0;0;;;;;;")
endif()

if(TARGET lotus-cfl-pocr)
    foreach(pocr_engine
            std-aa gspan-aa gr-aa grgspan-aa pocr-aa focr-aa
            std-vfa gspan-vfa gr-vfa grgspan-vfa pocr-vfa focr-vfa)
        if(pocr_engine MATCHES "-aa$")
            set(expected_engine_pairs 9)
        else()
            set(expected_engine_pairs 6)
        endif()
        add_test(NAME classical_cfl_engine_${pocr_engine}
            COMMAND $<TARGET_FILE:lotus-cfl-pocr>
                --engine ${pocr_engine}
                --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr.graph
                --query 0,2
                --json-stats)
        set_tests_properties(classical_cfl_engine_${pocr_engine} PROPERTIES
            PASS_REGULAR_EXPRESSION
            "\"result_pairs\":${expected_engine_pairs}")
    endforeach()
    add_test(NAME classical_cfl_engine_preprocessing
        COMMAND $<TARGET_FILE:lotus-cfl-pocr>
            --engine pocr-aa
            --graph ${CMAKE_SOURCE_DIR}/tests/regress/CFL/Classical/pocr-aa.graph
            --simplify-graph
            --interdyck
            --json-stats)
    set_tests_properties(classical_cfl_engine_preprocessing PROPERTIES
        PASS_REGULAR_EXPRESSION "\"folded_nodes\":")
endif()

get_property(cfl_registered_tests DIRECTORY PROPERTY TESTS)
foreach(cfl_registered_test IN LISTS cfl_registered_tests)
    if(cfl_registered_test MATCHES
       "^classical_cfl_(cli|alias|vf|foldability|engine|grammar)")
        set_tests_properties(${cfl_registered_test} PROPERTIES
            LABELS "lotus;integration;cfl;cli"
            TIMEOUT ${LOTUS_UNIT_TEST_TIMEOUT}
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    endif()
endforeach()

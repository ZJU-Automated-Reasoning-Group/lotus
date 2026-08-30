add_lotus_targeted_test(ClassicalCFLTest ClassicalCFLTest.cpp
    LINK_LIBS
        CanaryClassicalCFL)

add_lotus_targeted_test(scs_index_test SCSIndexTest.cpp
    LINK_LIBS
        CanaryCSIndex)

add_lotus_targeted_test(interleaved_dyck_approximation_test
    InterleavedDyckApproximationTest.cpp
    LINK_LIBS
        CanaryInterleavedDyckApproximation)

add_lotus_targeted_test(adaptive_interleaved_dyck_test
    AdaptiveInterleavedDyckTest.cpp
    LINK_LIBS
        CanaryAdaptiveInterleavedDyck)

add_lotus_targeted_test(interleaved_dyck_comparison_test
    InterleavedDyckComparisonTest.cpp
    LINK_LIBS
        CanaryAdaptiveInterleavedDyck
        CanaryInterleavedDyckApproximation
        CanaryMCFL)
target_compile_definitions(interleaved_dyck_comparison_test PRIVATE
    INTERLEAVED_DYCK_BENCHMARK_DIR="${CMAKE_SOURCE_DIR}/benchmarks/interleaved-dyck-approximation/taint")

add_lotus_targeted_test(mcfl_test MCFLTest.cpp
    LINK_LIBS
        CanaryMCFL)
target_compile_definitions(mcfl_test PRIVATE
    MCFL_BENCHMARK_DIR="${CMAKE_SOURCE_DIR}/benchmarks/interleaved-dyck-approximation/taint")

#add_lotus_targeted_test(ClassicalSVFPortTest ClassicalSVFPortTest.cpp
#    LINK_LIBS
#        CanaryClassicalCFL
#        SVFG
#        CanaryICFG)

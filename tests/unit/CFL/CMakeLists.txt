add_lotus_targeted_test(ClassicalCFLTest ClassicalCFLTest.cpp
    LINK_LIBS
        CanaryClassicalCFL)

add_lotus_targeted_test(scs_index_test SCSIndexTest.cpp
    LINK_LIBS
        CanaryCSIndex)

add_lotus_targeted_test(interleaved_dyck_test InterleavedDyckTest.cpp
    LINK_LIBS
        CanaryInterleavedDyck)

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

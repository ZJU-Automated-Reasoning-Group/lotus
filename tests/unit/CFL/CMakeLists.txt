add_lotus_targeted_test(ClassicalCFLTest ClassicalCFLTest.cpp
    LINK_LIBS
        CanaryClassicalCFL)

add_lotus_targeted_test(scs_index_test SCSIndexTest.cpp
    LINK_LIBS
        CanaryCSIndex)

add_lotus_targeted_test(interleaved_dyck_test InterleavedDyckTest.cpp
    LINK_LIBS
        CanaryInterleavedDyck)

#add_lotus_targeted_test(ClassicalSVFPortTest ClassicalSVFPortTest.cpp
#    LINK_LIBS
#        CanaryClassicalCFL
#        SVFG
#        CanaryICFG)

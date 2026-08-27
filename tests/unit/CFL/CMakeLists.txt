add_lotus_targeted_test(AriaCFLTest AriaCFLTest.cpp
    LINK_LIBS
        CanaryAriaCFL)

add_lotus_targeted_test(scs_index_test SCSIndexTest.cpp
    LINK_LIBS
        CanaryCSIndex)

add_lotus_targeted_test(interleaved_dyck_test InterleavedDyckTest.cpp
    LINK_LIBS
        CanaryInterleavedDyck)

#add_lotus_targeted_test(AriaSVFPortTest AriaSVFPortTest.cpp
#    LINK_LIBS
#        CanaryAriaCFL
#        SVFG
#        CanaryICFG)

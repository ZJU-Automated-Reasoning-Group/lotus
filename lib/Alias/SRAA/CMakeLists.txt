add_library(CanarySRAA STATIC
        RangeAnalysis.cpp
        StrictRelationAliasAnalysis.cpp
)
target_link_libraries(CanarySRAA PUBLIC AliasSpecManager)

add_library(CanaryDebugInfo STATIC
    DebugInfoAnalysis.cpp
    MetadataManager/MetadataEntry.cpp
    MetadataManager/LoopStructure.cpp
    MetadataManager/MetadataManager.cpp
    MetadataManager/MetadataManager_Module.cpp
    MetadataManager/MetadataManager_Instruction.cpp
    MetadataManager/MetadataManager_annotation.cpp
)
target_link_libraries(CanaryDebugInfo CanaryLLVMUtils)

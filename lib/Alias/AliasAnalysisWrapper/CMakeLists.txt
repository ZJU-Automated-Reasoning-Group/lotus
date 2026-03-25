llvm_map_components_to_libnames(cli_utils_llvm_libs Core IRReader Support)

# CLI utilities library for alias analysis command-line tools
add_library(CanaryAliasCLIUtils STATIC CLIUtils.cpp)
target_link_libraries(CanaryAliasCLIUtils PUBLIC AliasSpecManager ${cli_utils_llvm_libs})

# Use LLVM's CFL (from libLLVMAnalysis) instead of lotus's CFLAA to avoid duplicate
# symbols when linking with other libs that pull in LLVMAnalysis (e.g. CanaryLLVMUtils).
# Use LLVM's CFL from libLLVMAnalysis (LegacyPassManager is a header-only use here;
# the implementation is in the same translation unit as CFL, so Analysis suffices).
llvm_map_components_to_libnames(alias_wrapper_llvm_libs Analysis)

# Alias Analysis Wrapper library
add_library(CanaryAliasAnalysisWrapper STATIC
    AliasAnalysisWrapperCore.cpp
    AliasAnalysisWrapperQueries.cpp
    AliasAnalysisWrapperBackend.cpp
    AliasAnalysisFactory.cpp
    AliasAnalysisConfigParser.cpp
)

target_link_libraries(CanaryAliasAnalysisWrapper
    PUBLIC
        Andersen
        DDA
        CanaryDyckAA
        SeaDsaAnalysis
        CanaryAllocAA
        CanaryUnderApproxAA
        FSCSPointerAnalysis
        TPATransforms
        TPAUtil
        ${alias_wrapper_llvm_libs}
)

add_library(CanaryLLVMUtils STATIC
        Debug.cpp
        InstructionUtils.cpp
        RecursiveTimer.cpp
        Statistics.cpp
        StringUtils.cpp
        IO/FileUtils.cpp
        IO/ReadIR.cpp
        IO/WriteIR.cpp
        Demangle.cpp
)

# Link against LLVMDemangle since Demangle.cpp uses llvm::demangle()
target_link_libraries(CanaryLLVMUtils PUBLIC LLVMDemangle LLVMAnalysis CanarySupport)

# Define SeaDSA libraries
set(SEA_DSA_LIBS SeaDsaAnalysis)

# Define LLVM SeaHorn libraries (if any)
set(LLVM_SEAHORN_LIBS "")

add_subdirectory(seahorn)
add_subdirectory(seapp)
add_subdirectory(seainspect)

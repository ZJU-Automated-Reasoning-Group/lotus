# SMACK verifier integration for Lotus.
# Integrated from the SMACK source tree.

set(SMACK_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/Verification/smack)
set(SMACK_SEADSA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/Alias/UnificationBased)

add_library(SmackUtils STATIC
  utils/Devirt.cpp
  utils/MergeGEP.cpp
  utils/SimplifyExtractValue.cpp
  utils/SimplifyInsertValue.cpp
)

target_include_directories(SmackUtils PUBLIC
  ${SMACK_INCLUDE_DIR}
  ${SMACK_SEADSA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(SmackUtils PUBLIC
  SeaDsaAnalysis
)

target_compile_options(SmackUtils PRIVATE
  -Wno-deprecated-declarations
)

add_library(SmackTranslator STATIC
  smack/AddTiming.cpp
  smack/AnnotateLoopExits.cpp
  smack/BoogieAst.cpp
  smack/BplFilePrinter.cpp
  smack/BplPrinter.cpp
  smack/CodifyStaticInits.cpp
  smack/DSAWrapper.cpp
  smack/Debug.cpp
  smack/ExtractContracts.cpp
  smack/IntegerOverflowChecker.cpp
  smack/MemorySafetyChecker.cpp
  smack/Naming.cpp
  smack/NormalizeLoops.cpp
  smack/Prelude.cpp
  smack/Regions.cpp
  smack/RemoveDeadDefs.cpp
  smack/RewriteBitwiseOps.cpp
  smack/RustFixes.cpp
  smack/SimplifyLibCalls.cpp
  smack/SmackInstGenerator.cpp
  smack/SmackModuleGenerator.cpp
  smack/SmackOptions.cpp
  smack/SmackRep.cpp
  smack/SmackWarnings.cpp
  smack/SplitAggregateValue.cpp
  smack/VectorOperations.cpp
  smack/VerifierCodeMetadata.cpp
)

target_include_directories(SmackTranslator PUBLIC
  ${SMACK_INCLUDE_DIR}
  ${SMACK_SEADSA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(SmackTranslator PUBLIC
  SmackUtils
  SeaDsaAnalysis
)

target_compile_options(SmackTranslator PRIVATE
  -Wno-deprecated-declarations
)

install(TARGETS SmackUtils SmackTranslator
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
)

message(STATUS "SMACK libraries configured")

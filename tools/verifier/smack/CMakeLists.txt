# SMACK verifier tools for Lotus.

set(SMACK_INCLUDE_DIR
    ${CMAKE_SOURCE_DIR}/third-party/verification/smack/include/Verification/smack)
set(SMACK_SEADSA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include/Alias/UnificationBased)

add_library(SmackExternalizePass STATIC
  externalizer/ExternalizePass.cpp
)

target_include_directories(SmackExternalizePass PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/externalizer
  ${SMACK_INCLUDE_DIR}
  ${SMACK_SEADSA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_options(SmackExternalizePass PRIVATE
  -Wno-deprecated-declarations
)

add_executable(llvm2bpl
  llvm2bpl/llvm2bpl.cpp
)

target_include_directories(llvm2bpl PRIVATE
  ${SMACK_INCLUDE_DIR}
  ${SMACK_SEADSA_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

llvm_config(llvm2bpl
  analysis
  bitreader
  bitwriter
  core
  ipo
  irreader
  native
  scalaropts
  support
  transformutils
)

target_link_libraries(llvm2bpl PRIVATE
  SmackTranslator
  SmackUtils
  SeaDsaAnalysis
)

target_compile_options(llvm2bpl PRIVATE
  -Wno-deprecated-declarations
)

add_executable(extern-statics
  externalizer/extern-statics.cpp
)

target_include_directories(extern-statics PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/externalizer
  ${SMACK_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

llvm_config(extern-statics
  bitreader
  bitwriter
  core
  irreader
  support
  transformutils
)

target_link_libraries(extern-statics PRIVATE
  SmackExternalizePass
)

target_compile_options(extern-statics PRIVATE
  -Wno-deprecated-declarations
)

install(TARGETS llvm2bpl extern-statics
  RUNTIME DESTINATION bin
)

install(PROGRAMS
  bin/smack
  bin/smack-doctor
  bin/smack-svcomp-wrapper.sh
  DESTINATION bin
)

install(DIRECTORY share/smack
  DESTINATION share
  USE_SOURCE_PERMISSIONS
  FILES_MATCHING
    PATTERN "*.py"
    PATTERN "*.h"
    PATTERN "*.c"
    PATTERN "Makefile"
    PATTERN "*.rs"
    PATTERN "*.f90"
    PATTERN "*.di"
    PATTERN "*.toml"
    PATTERN "*.cpp"
    PATTERN "cassert"
)

install(FILES bin/versions
  DESTINATION share/smack
  RENAME versions.py
)

message(STATUS "SMACK tools configured: llvm2bpl, extern-statics")

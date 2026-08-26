# Analysis Unit Tests

set(NOELLE_LOOP_PARITY_SRC_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/noelle_parity)
set(NOELLE_LOOP_PARITY_LL_DIR
    ${CMAKE_BINARY_DIR}/tests/unit/noelle_loop_parity_ll)

file(MAKE_DIRECTORY ${NOELLE_LOOP_PARITY_LL_DIR})

if(APPLE)
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/clang++)
    set(NOELLE_LOOP_PARITY_CLANGXX /opt/homebrew/opt/llvm@14/bin/clang++)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/clang++)
    set(NOELLE_LOOP_PARITY_CLANGXX /usr/local/opt/llvm@14/bin/clang++)
  else()
    execute_process(
        COMMAND xcrun --sdk macosx --find clang++
        OUTPUT_VARIABLE NOELLE_LOOP_PARITY_CLANGXX
        OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/opt)
    set(NOELLE_LOOP_PARITY_OPT /opt/homebrew/opt/llvm@14/bin/opt)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/opt)
    set(NOELLE_LOOP_PARITY_OPT /usr/local/opt/llvm@14/bin/opt)
  else()
    find_program(NOELLE_LOOP_PARITY_OPT opt REQUIRED)
  endif()
  execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE NOELLE_LOOP_PARITY_SYSROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(NOELLE_LOOP_PARITY_EXTRA_ARGS -isysroot ${NOELLE_LOOP_PARITY_SYSROOT})
else()
  find_program(NOELLE_LOOP_PARITY_CLANGXX
               NAMES clang++-14 clang++
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR})
  if(NOT NOELLE_LOOP_PARITY_CLANGXX)
    set(NOELLE_LOOP_PARITY_CLANGXX ${CMAKE_CXX_COMPILER})
  endif()
  find_program(NOELLE_LOOP_PARITY_OPT
               NAMES opt-14 opt
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR}
               REQUIRED)
  set(NOELLE_LOOP_PARITY_EXTRA_ARGS)
endif()

set(NOELLE_LOOP_PARITY_CASES
    iv_attributes/nested_loop_governing
    sccdag_attributes/simple
    sccdag_attributes/reducible_addition
    sccdag_attributes/clonable_alloca
    loop_domain_space/one_dim_array
    loop_domain_space/two_dim_matrix)

set(NOELLE_LOOP_PARITY_LL_OUTPUTS)
foreach(case_name ${NOELLE_LOOP_PARITY_CASES})
  set(src_file ${NOELLE_LOOP_PARITY_SRC_DIR}/${case_name}/test.cpp)
  string(REPLACE "/" "_" case_output ${case_name})
  set(pre_bc ${NOELLE_LOOP_PARITY_LL_DIR}/${case_output}_pre.bc)
  set(ll_file ${NOELLE_LOOP_PARITY_LL_DIR}/${case_output}.ll)
  list(APPEND NOELLE_LOOP_PARITY_LL_OUTPUTS ${ll_file})
  add_custom_command(
      OUTPUT ${ll_file}
      COMMAND ${NOELLE_LOOP_PARITY_CLANGXX}
              ${NOELLE_LOOP_PARITY_EXTRA_ARGS}
              -std=c++17 -emit-llvm -c -g -O0 -Xclang -disable-O0-optnone
              ${src_file} -o ${pre_bc}
      COMMAND ${NOELLE_LOOP_PARITY_OPT}
              -basic-aa -mem2reg -scalar-evolution -loops
              -loop-simplify -lcssa -domtree -postdomtree
              -S ${pre_bc} -o ${ll_file}
      DEPENDS ${src_file}
      COMMENT "Generating LLVM IR for NOELLE parity fixture ${case_name}"
      VERBATIM)
endforeach()

add_custom_target(noelle_loop_parity_ll_fixtures DEPENDS ${NOELLE_LOOP_PARITY_LL_OUTPUTS})

add_subdirectory(CFG)
add_subdirectory(ControlDependence)
add_subdirectory(DebugInfo)
add_subdirectory(General)
add_subdirectory(Loop)
add_subdirectory(Multiplicity)
add_subdirectory(NullPointer)
add_subdirectory(ParameterSummary)
add_subdirectory(Profile)
add_subdirectory(Purity)
add_subdirectory(SCCP)

add_lotus_test_suite(analysis_misc_tests
  SOURCES
    CFG/CFGUtilitiesTest.cpp
    CFG/ICFGTest.cpp
    Multiplicity/MultiplicityClassifierTest.cpp
    ParameterSummary/ParameterEffectSummaryTest.cpp
    Profile/HotProfileTest.cpp
    SCCP/SCCPTest.cpp
  LINK_LIBS
    CanaryCFG
    CanaryICFG
    CanaryMultiplicity
    CanaryParameterSummary
    CanaryProfile
    CanarySCCP
    LLVMAnalysis)

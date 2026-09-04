# CMakeLists.txt for TypeHierarchy unit tests

set(TYPE_HIERARCHY_SRC_DIR
    ${CMAKE_SOURCE_DIR}/tests/regress/Analysis/TypeHierarchy)
set(TYPE_HIERARCHY_LL_DIR
    ${CMAKE_BINARY_DIR}/tests/unit/type_hierarchy_ll)

file(MAKE_DIRECTORY ${TYPE_HIERARCHY_LL_DIR})

if(APPLE)
  if(EXISTS /opt/homebrew/opt/llvm@14/bin/clang++)
    set(TYPE_HIERARCHY_CLANGXX /opt/homebrew/opt/llvm@14/bin/clang++)
  elseif(EXISTS /usr/local/opt/llvm@14/bin/clang++)
    set(TYPE_HIERARCHY_CLANGXX /usr/local/opt/llvm@14/bin/clang++)
  else()
    execute_process(
        COMMAND xcrun --sdk macosx --find clang++
        OUTPUT_VARIABLE TYPE_HIERARCHY_CLANGXX
        OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  execute_process(
      COMMAND xcrun --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE TYPE_HIERARCHY_SYSROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS -isysroot ${TYPE_HIERARCHY_SYSROOT})
else()
  find_program(TYPE_HIERARCHY_CLANGXX
               NAMES clang++-14 clang++
               HINTS ${LOTUS_LLVM_TOOLS_BINARY_DIR})
  if(NOT TYPE_HIERARCHY_CLANGXX)
    set(TYPE_HIERARCHY_CLANGXX ${CMAKE_CXX_COMPILER})
  endif()
  set(TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS)
endif()

set(TYPE_HIERARCHY_CASES
    type_hierarchy_1
    type_hierarchy_2
    type_hierarchy_3
    type_hierarchy_4
    type_hierarchy_5
    type_hierarchy_6
    type_hierarchy_7
    type_hierarchy_7_b
    type_hierarchy_8
    type_hierarchy_9
    type_hierarchy_10
    type_hierarchy_11
    type_hierarchy_12
    type_hierarchy_12_b
    type_hierarchy_12_c
    type_hierarchy_13
    type_hierarchy_14
    type_hierarchy_15
    type_hierarchy_16
    type_hierarchy_17
    type_hierarchy_18
    type_hierarchy_19
    type_hierarchy_20
    type_hierarchy_21
    type_hierarchy_22)

set(TYPE_HIERARCHY_LL_OUTPUTS)
foreach(case_name ${TYPE_HIERARCHY_CASES})
  set(src_file ${TYPE_HIERARCHY_SRC_DIR}/${case_name}.cpp)
  set(ll_file ${TYPE_HIERARCHY_LL_DIR}/${case_name}_cpp_dbg.ll)
  list(APPEND TYPE_HIERARCHY_LL_OUTPUTS ${ll_file})
  add_custom_command(
      OUTPUT ${ll_file}
      COMMAND ${TYPE_HIERARCHY_CLANGXX}
              ${TYPE_HIERARCHY_CLANGXX_EXTRA_ARGS}
              -std=c++17 -S -emit-llvm -g -O0
              ${src_file} -o ${ll_file}
      DEPENDS ${src_file}
      COMMENT "Generating debug LLVM IR for ${case_name}"
      VERBATIM)
endforeach()

add_custom_target(type_hierarchy_ll_fixtures DEPENDS ${TYPE_HIERARCHY_LL_OUTPUTS})

add_lotus_test_suite(type_hierarchy_tests
  TEST_KIND INTEGRATION
  SOURCES
    DIBasedTypeHierarchyTest.cpp
    DIBasedTypeHierarchyTransitiveReachabilityTest.cpp
    DIBasedTypeHierarchySerializationTest.cpp
    LLVMVFTableTest.cpp
  LINK_LIBS CanaryTypeHierarchy
  COMPILE_DEFINITIONS
    LOTUS_TYPE_HIERARCHY_LL_DIR="${TYPE_HIERARCHY_LL_DIR}"
  DEPENDS type_hierarchy_ll_fixtures)

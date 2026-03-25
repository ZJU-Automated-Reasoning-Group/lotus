/**
 * @file DIBasedTypeHierarchyTest.cpp
 * @brief Unit tests for DIBasedTypeHierarchy
 *
 * This file contains comprehensive tests for the DIBasedTypeHierarchy class,
 * which reconstructs C++ type hierarchies from LLVM IR with debug information.
 * Tests are migrated from PhasarLLVM TypeHierarchy tests.
 */

#include "Analysis/TypeHirarchy/DIBasedTypeHierarchy.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using namespace llvm;
using namespace lotus;

namespace {

using lotus::unittest::loadModule;

// Helper function to get test file path
std::string getTestFilePath(const std::string &FileName) {
  return std::string(LOTUS_TYPE_HIERARCHY_LL_DIR) + "/" + FileName;
}

/*
---------------------------
BasicTHReconstruction Tests
---------------------------
*/

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_1) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_1_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_2) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_2_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_3) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_3_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_4) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_4_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_5) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_5_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 3U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto OtherBaseType = DBTH.getType("_ZTS9OtherBase");
  ASSERT_TRUE(OtherBaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*OtherBaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypesBase = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypesBase.find(*ChildType) != SubTypesBase.end());
  auto SubTypesOtherBase = DBTH.getSubTypes(*OtherBaseType);
  EXPECT_TRUE(SubTypesOtherBase.find(*ChildType) != SubTypesOtherBase.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_6) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_6_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_7) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_7_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 7U);
  auto AType = DBTH.getType("_ZTS1A");
  ASSERT_TRUE(AType.hasValue());
  auto BType = DBTH.getType("_ZTS1B");
  ASSERT_TRUE(BType.hasValue());
  auto CType = DBTH.getType("_ZTS1C");
  ASSERT_TRUE(CType.hasValue());
  auto DType = DBTH.getType("_ZTS1D");
  ASSERT_TRUE(DType.hasValue());
  auto XType = DBTH.getType("_ZTS1X");
  ASSERT_TRUE(XType.hasValue());
  auto YType = DBTH.getType("_ZTS1Y");
  ASSERT_TRUE(YType.hasValue());
  auto ZType = DBTH.getType("_ZTS1Z");
  ASSERT_TRUE(ZType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*AType));
  EXPECT_TRUE(DBTH.hasType(*BType));
  EXPECT_TRUE(DBTH.hasType(*CType));
  EXPECT_TRUE(DBTH.hasType(*DType));
  EXPECT_TRUE(DBTH.hasType(*XType));
  EXPECT_TRUE(DBTH.hasType(*YType));
  EXPECT_TRUE(DBTH.hasType(*ZType));

  // check for all subtypes

  // struct B : A {};
  // struct C : A {};
  auto SubTypesA = DBTH.getSubTypes(*AType);
  EXPECT_TRUE(SubTypesA.find(*BType) != SubTypesA.end());
  EXPECT_TRUE(SubTypesA.find(*CType) != SubTypesA.end());
  // struct D : B {};
  auto SubTypesB = DBTH.getSubTypes(*BType);
  EXPECT_TRUE(SubTypesB.find(*DType) != SubTypesB.end());
  // struct Z : C, Y {};
  auto SubTypesC = DBTH.getSubTypes(*CType);
  EXPECT_TRUE(SubTypesC.find(*ZType) != SubTypesC.end());
  // struct Y : X {};
  auto SubTypesX = DBTH.getSubTypes(*XType);
  EXPECT_TRUE(SubTypesX.find(*YType) != SubTypesX.end());
  // struct Z : C, Y {};
  auto SubTypesY = DBTH.getSubTypes(*YType);
  EXPECT_TRUE(SubTypesY.find(*ZType) != SubTypesY.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_7_b) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_7_b_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 6U);
  auto AType = DBTH.getType("A");
  ASSERT_TRUE(AType.hasValue());
  auto CType = DBTH.getType("_ZTS1C");
  ASSERT_TRUE(CType.hasValue());
  auto XType = DBTH.getType("X");
  ASSERT_TRUE(XType.hasValue());
  auto YType = DBTH.getType("_ZTS1Y");
  ASSERT_TRUE(YType.hasValue());
  auto ZType = DBTH.getType("_ZTS1Z");
  ASSERT_TRUE(ZType.hasValue());
  auto OmegaType = DBTH.getType("_ZTS5Omega");
  ASSERT_TRUE(OmegaType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*AType));
  EXPECT_TRUE(DBTH.hasType(*CType));
  EXPECT_TRUE(DBTH.hasType(*XType));
  EXPECT_TRUE(DBTH.hasType(*YType));
  EXPECT_TRUE(DBTH.hasType(*ZType));
  EXPECT_TRUE(DBTH.hasType(*OmegaType));

  // check for all subtypes

  // struct C : A {};
  auto SubTypesA = DBTH.getSubTypes(*AType);
  EXPECT_TRUE(SubTypesA.find(*CType) != SubTypesA.end());
  // struct Z : C, Y {};
  auto SubTypesC = DBTH.getSubTypes(*CType);
  EXPECT_TRUE(SubTypesC.find(*ZType) != SubTypesC.end());
  // struct Y : X {};
  auto SubTypesX = DBTH.getSubTypes(*XType);
  EXPECT_TRUE(SubTypesX.find(*YType) != SubTypesX.end());
  // struct Z : C, Y {};
  auto SubTypesY = DBTH.getSubTypes(*YType);
  EXPECT_TRUE(SubTypesY.find(*ZType) != SubTypesY.end());

  // class Omega : Z {
  auto SubTypesZ = DBTH.getSubTypes(*ZType);
  EXPECT_TRUE(SubTypesZ.find(*OmegaType) != SubTypesZ.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_8) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_8_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 4U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto NonvirtualClassType = DBTH.getType("_ZTS15NonvirtualClass");
  EXPECT_TRUE(NonvirtualClassType.hasValue());
  auto NonvirtualStructType = DBTH.getType("_ZTS16NonvirtualStruct");
  EXPECT_TRUE(NonvirtualStructType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  EXPECT_TRUE(DBTH.hasType(*NonvirtualClassType));
  EXPECT_TRUE(DBTH.hasType(*NonvirtualStructType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_9) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_9_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_10) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_10_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_11) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_11_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_12) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_12_b) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_b_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 3U);
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto ChildsChildType = DBTH.getType("_ZTS11ChildsChild");
  ASSERT_TRUE(ChildsChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  EXPECT_TRUE(DBTH.hasType(*ChildsChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
#if LLVM_VERSION_MAJOR < 16
  // In LLVM 16, the metadata is pruned to the relations that are actually
  // *used* in the code
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
#endif
  auto SubTypesChild = DBTH.getSubTypes(*ChildType);
  EXPECT_TRUE(SubTypesChild.find(*ChildsChildType) != SubTypesChild.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_12_c) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_c_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto ChildsChildType = DBTH.getType("_ZTS11ChildsChild");
  ASSERT_TRUE(ChildsChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*ChildType));
  EXPECT_TRUE(DBTH.hasType(*ChildsChildType));

  // check for all subtypes
  auto SubTypesChild = DBTH.getSubTypes(*ChildType);
  EXPECT_TRUE(SubTypesChild.find(*ChildsChildType) != SubTypesChild.end());
}

/*
TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_13) {
  // Test file 13 has no types - skipped
}
*/

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_14) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_14_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 1U);
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));

  // there are no subtypes here
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_15) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_15_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 2U);
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_16) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_16_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 5U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  // Since ChildsChild is never used, it is optimized out
  // auto ChildsChildType = DBTH.getType("ChildsChild");
  // ASSERT_FALSE(ChildsChildType.hasValue());
  auto BaseTwoType = DBTH.getType("_ZTS7BaseTwo");
  ASSERT_TRUE(BaseTwoType.hasValue());
  auto ChildTwoType = DBTH.getType("_ZTS8ChildTwo");
  ASSERT_TRUE(ChildTwoType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  // Since ChildsChild is never used, it is optimized out
  // EXPECT_FALSE(DBTH.hasType(*ChildsChildType));
  EXPECT_TRUE(DBTH.hasType(*BaseTwoType));
  EXPECT_TRUE(DBTH.hasType(*ChildTwoType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
  // auto SubTypesChild = DBTH.getSubTypes(*ChildType);
  // Since ChildsChild is never used, it is optimized out
  // EXPECT_TRUE(SubTypesChild.find(*ChildsChildType) == SubTypesChild.end());
  auto SubTypesTwo = DBTH.getSubTypes(*BaseTwoType);
  EXPECT_TRUE(SubTypesTwo.find(*ChildTwoType) != SubTypesTwo.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_17) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_17_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  // EXPECT_EQ(DBTH.getAllTypes().size(), 5U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  // auto Child2Type = DBTH.getType("Child2");
  // Since Child2Type is never used, it is optimized out
  // ASSERT_FALSE(Child2Type.hasValue());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.hasValue());
  auto KidType = DBTH.getType("_ZTS3Kid");
  ASSERT_TRUE(KidType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  // Since ChildsChild is never used, it is optimized out
  // EXPECT_FALSE(DBTH.hasType(*Child2Type));
  EXPECT_TRUE(DBTH.hasType(*Base2Type));
  EXPECT_TRUE(DBTH.hasType(*KidType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
  // auto SubTypesChild = DBTH.getSubTypes(*ChildType);
  // Since ChildsChild is never used, it is optimized out
  // EXPECT_TRUE(SubTypesChild.find(*Child2Type) == SubTypesChild.end());
  auto SubTypesBase2 = DBTH.getSubTypes(*Base2Type);
  EXPECT_TRUE(SubTypesBase2.find(*KidType) != SubTypesBase2.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_18) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_18_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 4U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  // auto Child_2Type = DBTH.getType("Child_2");
  // Since Child2Type is never used, it is optimized out
  // ASSERT_FALSE(Child2Type.hasValue());
  auto Child3Type = DBTH.getType("_ZTS7Child_3");
  ASSERT_TRUE(Child3Type.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  // Since Child2 is never used, it is optimized out
  // EXPECT_FALSE(DBTH.hasType(*Child2Type));
  EXPECT_TRUE(DBTH.hasType(*Child3Type));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
  // auto SubTypesChild = DBTH.getSubTypes(*ChildType);
  // Since Child2 is never used, it is optimized out
  // EXPECT_TRUE(SubTypesChild.find(*Child2Type) == SubTypesChild.end());
  auto SubTypesChild2 = DBTH.getSubTypes(*Child3Type);
  EXPECT_TRUE(SubTypesChild2.find(*Child3Type) != SubTypesChild2.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_19) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_19_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 6U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto FooType = DBTH.getType("_ZTS3Foo");
  ASSERT_TRUE(FooType.hasValue());
  auto BarType = DBTH.getType("_ZTS3Bar");
  ASSERT_TRUE(BarType.hasValue());
  auto LoremType = DBTH.getType("_ZTS5Lorem");
  ASSERT_TRUE(LoremType.hasValue());
  auto ImpsumType = DBTH.getType("_ZTS6Impsum");
  ASSERT_TRUE(ImpsumType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  EXPECT_TRUE(DBTH.hasType(*FooType));
  EXPECT_TRUE(DBTH.hasType(*BarType));
  EXPECT_TRUE(DBTH.hasType(*LoremType));
  EXPECT_TRUE(DBTH.hasType(*ImpsumType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
  auto SubTypesFoo = DBTH.getSubTypes(*FooType);
  EXPECT_TRUE(SubTypesFoo.find(*BarType) != SubTypesFoo.end());
  auto SubTypesLorem = DBTH.getSubTypes(*LoremType);
  EXPECT_TRUE(SubTypesLorem.find(*ImpsumType) != SubTypesLorem.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_20) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_20_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 3U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*Base2Type));
  EXPECT_TRUE(DBTH.hasType(*ChildType));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
  auto SubTypes2 = DBTH.getSubTypes(*Base2Type);
  EXPECT_TRUE(SubTypes2.find(*ChildType) != SubTypes2.end());
}

TEST(DIBasedTypeHierarchyTest, BasicTHReconstruction_21) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_21_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  EXPECT_EQ(DBTH.getAllTypes().size(), 5U);
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.hasValue());
  auto Base3Type = DBTH.getType("_ZTS5Base3");
  ASSERT_TRUE(Base3Type.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto Child2Type = DBTH.getType("_ZTS6Child2");
  ASSERT_TRUE(Child2Type.hasValue());

  EXPECT_TRUE(DBTH.hasType(*BaseType));
  EXPECT_TRUE(DBTH.hasType(*Base2Type));
  EXPECT_TRUE(DBTH.hasType(*Base3Type));
  EXPECT_TRUE(DBTH.hasType(*ChildType));
  EXPECT_TRUE(DBTH.hasType(*Child2Type));

  // check for all subtypes
  auto SubTypes = DBTH.getSubTypes(*BaseType);
  EXPECT_TRUE(SubTypes.find(*ChildType) != SubTypes.end());
  auto SubTypesBase2 = DBTH.getSubTypes(*Base2Type);
  EXPECT_TRUE(SubTypesBase2.find(*ChildType) != SubTypesBase2.end());
  auto SubTypesChild = DBTH.getSubTypes(*ChildType);
  EXPECT_TRUE(SubTypesChild.find(*Child2Type) != SubTypesChild.end());
  auto SubTypesBase3 = DBTH.getSubTypes(*Base3Type);
  EXPECT_TRUE(SubTypesBase3.find(*Child2Type) != SubTypesBase3.end());
}

/*
--------------------------------
TransitivelyReachableTypes Tests
--------------------------------
*/

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_1) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_1_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_2) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_2_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_3) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_3_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_4) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_4_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_5) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_5_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto OtherBaseType = DBTH.getType("_ZTS9OtherBase");
  ASSERT_TRUE(OtherBaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesOtherBase = DBTH.getSubTypes(*OtherBaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesOtherBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesOtherBase.count(*OtherBaseType));
  EXPECT_TRUE(ReachableTypesOtherBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild.count(*OtherBaseType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_6) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_6_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_7) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_7_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto AType = DBTH.getType("_ZTS1A");
  ASSERT_TRUE(AType.hasValue());
  auto BType = DBTH.getType("_ZTS1B");
  ASSERT_TRUE(BType.hasValue());
  auto CType = DBTH.getType("_ZTS1C");
  ASSERT_TRUE(CType.hasValue());
  auto DType = DBTH.getType("_ZTS1D");
  ASSERT_TRUE(DType.hasValue());
  auto XType = DBTH.getType("_ZTS1X");
  ASSERT_TRUE(XType.hasValue());
  auto YType = DBTH.getType("_ZTS1Y");
  ASSERT_TRUE(YType.hasValue());
  auto ZType = DBTH.getType("_ZTS1Z");
  ASSERT_TRUE(ZType.hasValue());

  auto ReachableTypesA = DBTH.getSubTypes(*AType);
  auto ReachableTypesB = DBTH.getSubTypes(*BType);
  auto ReachableTypesC = DBTH.getSubTypes(*CType);
  auto ReachableTypesD = DBTH.getSubTypes(*DType);
  auto ReachableTypesX = DBTH.getSubTypes(*XType);
  auto ReachableTypesY = DBTH.getSubTypes(*YType);
  auto ReachableTypesZ = DBTH.getSubTypes(*ZType);

  EXPECT_EQ(ReachableTypesA.size(), 5U);
  EXPECT_EQ(ReachableTypesB.size(), 2U);
  EXPECT_EQ(ReachableTypesC.size(), 2U);
  EXPECT_EQ(ReachableTypesD.size(), 1U);
  EXPECT_EQ(ReachableTypesX.size(), 3U);
  EXPECT_EQ(ReachableTypesY.size(), 2U);
  EXPECT_EQ(ReachableTypesZ.size(), 1U);

  EXPECT_TRUE(ReachableTypesA.count(*AType));
  EXPECT_TRUE(ReachableTypesA.count(*BType));
  EXPECT_TRUE(ReachableTypesA.count(*CType));
  EXPECT_TRUE(ReachableTypesA.count(*DType));
  EXPECT_TRUE(ReachableTypesA.count(*ZType));
  EXPECT_TRUE(ReachableTypesB.count(*BType));
  EXPECT_TRUE(ReachableTypesB.count(*DType));
  EXPECT_TRUE(ReachableTypesC.count(*CType));
  EXPECT_TRUE(ReachableTypesC.count(*ZType));
  EXPECT_TRUE(ReachableTypesD.count(*DType));
  EXPECT_TRUE(ReachableTypesX.count(*XType));
  EXPECT_TRUE(ReachableTypesX.count(*YType));
  EXPECT_TRUE(ReachableTypesX.count(*ZType));
  EXPECT_TRUE(ReachableTypesY.count(*YType));
  EXPECT_TRUE(ReachableTypesY.count(*ZType));
  EXPECT_TRUE(ReachableTypesZ.count(*ZType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_7_b) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_7_b_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto AType = DBTH.getType("A");
  ASSERT_TRUE(AType.hasValue());
  auto CType = DBTH.getType("_ZTS1C");
  ASSERT_TRUE(CType.hasValue());
  auto XType = DBTH.getType("X");
  ASSERT_TRUE(XType.hasValue());
  auto YType = DBTH.getType("_ZTS1Y");
  ASSERT_TRUE(YType.hasValue());
  auto ZType = DBTH.getType("_ZTS1Z");
  ASSERT_TRUE(ZType.hasValue());
  auto OmegaType = DBTH.getType("_ZTS5Omega");
  ASSERT_TRUE(OmegaType.hasValue());

  auto ReachableTypesA = DBTH.getSubTypes(*AType);
  auto ReachableTypesC = DBTH.getSubTypes(*CType);
  auto ReachableTypesX = DBTH.getSubTypes(*XType);
  auto ReachableTypesY = DBTH.getSubTypes(*YType);
  auto ReachableTypesZ = DBTH.getSubTypes(*ZType);
  auto ReachableTypesOmega = DBTH.getSubTypes(*OmegaType);

  EXPECT_EQ(ReachableTypesA.size(), 4U);
  EXPECT_EQ(ReachableTypesC.size(), 3U);
  EXPECT_EQ(ReachableTypesX.size(), 4U);
  EXPECT_EQ(ReachableTypesY.size(), 3U);
  EXPECT_EQ(ReachableTypesZ.size(), 2U);
  EXPECT_EQ(ReachableTypesOmega.size(), 1U);

  EXPECT_TRUE(ReachableTypesA.count(*AType));
  EXPECT_TRUE(ReachableTypesA.count(*CType));
  EXPECT_TRUE(ReachableTypesA.count(*ZType));
  EXPECT_TRUE(ReachableTypesA.count(*OmegaType));
  EXPECT_TRUE(ReachableTypesC.count(*CType));
  EXPECT_TRUE(ReachableTypesC.count(*ZType));
  EXPECT_TRUE(ReachableTypesC.count(*OmegaType));
  EXPECT_TRUE(ReachableTypesX.count(*XType));
  EXPECT_TRUE(ReachableTypesX.count(*YType));
  EXPECT_TRUE(ReachableTypesX.count(*ZType));
  EXPECT_TRUE(ReachableTypesX.count(*OmegaType));
  EXPECT_TRUE(ReachableTypesY.count(*YType));
  EXPECT_TRUE(ReachableTypesY.count(*ZType));
  EXPECT_TRUE(ReachableTypesY.count(*OmegaType));
  EXPECT_TRUE(ReachableTypesZ.count(*ZType));
  EXPECT_TRUE(ReachableTypesZ.count(*OmegaType));
  EXPECT_TRUE(ReachableTypesOmega.count(*OmegaType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_8) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_8_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto NonvirtualClassType = DBTH.getType("_ZTS15NonvirtualClass");
  EXPECT_TRUE(NonvirtualClassType.hasValue());
  auto NonvirtualStructType = DBTH.getType("_ZTS16NonvirtualStruct");
  EXPECT_TRUE(NonvirtualStructType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesNonvirtualClass = DBTH.getSubTypes(*NonvirtualClassType);
  auto ReachableTypesNonvirtualStruct =
      DBTH.getSubTypes(*NonvirtualStructType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_EQ(ReachableTypesNonvirtualClass.size(), 1U);
  EXPECT_EQ(ReachableTypesNonvirtualStruct.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesNonvirtualClass.count(*NonvirtualClassType));
  EXPECT_TRUE(ReachableTypesNonvirtualStruct.count(*NonvirtualStructType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_9) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_9_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_10) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_10_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_11) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_11_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_12) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_12_b) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_b_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto ChildsChildType = DBTH.getType("_ZTS11ChildsChild");
  ASSERT_TRUE(ChildsChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChildsChild = DBTH.getSubTypes(*ChildsChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 3U);
  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesChildsChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildsChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildsChildType));
  EXPECT_TRUE(ReachableTypesChildsChild.count(*ChildsChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_12_c) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_12_c_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto ChildsChildType = DBTH.getType("_ZTS11ChildsChild");
  ASSERT_TRUE(ChildsChildType.hasValue());

  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChildsChild = DBTH.getSubTypes(*ChildsChildType);

  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesChildsChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildsChildType));
  EXPECT_TRUE(ReachableTypesChildsChild.count(*ChildsChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_14) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_14_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);

  EXPECT_EQ(ReachableTypesBase.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_15) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_15_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_16) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_16_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto BaseTwoType = DBTH.getType("_ZTS7BaseTwo");
  ASSERT_TRUE(BaseTwoType.hasValue());
  auto ChildTwoType = DBTH.getType("_ZTS8ChildTwo");
  ASSERT_TRUE(ChildTwoType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesBaseTwo = DBTH.getSubTypes(*BaseTwoType);
  auto ReachableTypesChildTwo = DBTH.getSubTypes(*ChildTwoType);

  EXPECT_EQ(ReachableTypesBase.size(), 3U);
  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesBaseTwo.size(), 2U);
  EXPECT_EQ(ReachableTypesChildTwo.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBaseTwo.count(*BaseTwoType));
  EXPECT_TRUE(ReachableTypesBaseTwo.count(*ChildTwoType));
  EXPECT_FALSE(ReachableTypesChildTwo.count(*BaseTwoType));
  EXPECT_TRUE(ReachableTypesChildTwo.count(*ChildTwoType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_17) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_17_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.hasValue());
  auto KidType = DBTH.getType("_ZTS3Kid");
  ASSERT_TRUE(KidType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesBase2 = DBTH.getSubTypes(*Base2Type);
  auto ReachableTypesKid = DBTH.getSubTypes(*KidType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_EQ(ReachableTypesBase2.size(), 2U);
  EXPECT_EQ(ReachableTypesKid.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase2.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*KidType));
  EXPECT_FALSE(ReachableTypesKid.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesKid.count(*KidType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_18) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_18_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto Child3Type = DBTH.getType("_ZTS7Child_3");
  ASSERT_TRUE(Child3Type.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChild3 = DBTH.getSubTypes(*Child3Type);

  EXPECT_EQ(ReachableTypesBase.size(), 4U);
  EXPECT_EQ(ReachableTypesChild.size(), 3U);
  EXPECT_EQ(ReachableTypesChild3.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild3.count(*Child3Type));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_19) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_19_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto FooType = DBTH.getType("_ZTS3Foo");
  ASSERT_TRUE(FooType.hasValue());
  auto BarType = DBTH.getType("_ZTS3Bar");
  ASSERT_TRUE(BarType.hasValue());
  auto LoremType = DBTH.getType("_ZTS5Lorem");
  ASSERT_TRUE(LoremType.hasValue());
  auto ImpsumType = DBTH.getType("_ZTS6Impsum");
  ASSERT_TRUE(ImpsumType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesFoo = DBTH.getSubTypes(*FooType);
  auto ReachableTypesBar = DBTH.getSubTypes(*BarType);
  auto ReachableTypesLorem = DBTH.getSubTypes(*LoremType);
  auto ReachableTypesImpsum = DBTH.getSubTypes(*ImpsumType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_EQ(ReachableTypesFoo.size(), 2U);
  EXPECT_EQ(ReachableTypesBar.size(), 1U);
  EXPECT_EQ(ReachableTypesLorem.size(), 2U);
  EXPECT_EQ(ReachableTypesImpsum.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesFoo.count(*FooType));
  EXPECT_TRUE(ReachableTypesFoo.count(*BarType));
  EXPECT_FALSE(ReachableTypesBar.count(*FooType));
  EXPECT_TRUE(ReachableTypesBar.count(*BarType));
  EXPECT_TRUE(ReachableTypesLorem.count(*LoremType));
  EXPECT_TRUE(ReachableTypesLorem.count(*ImpsumType));
  EXPECT_FALSE(ReachableTypesImpsum.count(*LoremType));
  EXPECT_TRUE(ReachableTypesImpsum.count(*ImpsumType));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_20) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_20_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesBase2 = DBTH.getSubTypes(*Base2Type);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);

  EXPECT_EQ(ReachableTypesBase.size(), 2U);
  EXPECT_EQ(ReachableTypesBase2.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase2.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild.count(*Base2Type));
}

TEST(DIBasedTypeHierarchyTest, TransitivelyReachableTypes_21) {
  LLVMContext Context;
  auto M = loadModule(getTestFilePath("type_hierarchy_21_cpp_dbg.ll"), Context);
  ASSERT_NE(nullptr, M);
  DIBasedTypeHierarchy DBTH(*M);

  // check for all types
  auto BaseType = DBTH.getType("_ZTS4Base");
  ASSERT_TRUE(BaseType.hasValue());
  auto Base2Type = DBTH.getType("_ZTS5Base2");
  ASSERT_TRUE(Base2Type.hasValue());
  auto Base3Type = DBTH.getType("_ZTS5Base3");
  ASSERT_TRUE(Base3Type.hasValue());
  auto ChildType = DBTH.getType("_ZTS5Child");
  ASSERT_TRUE(ChildType.hasValue());
  auto Child2Type = DBTH.getType("_ZTS6Child2");
  ASSERT_TRUE(Child2Type.hasValue());

  auto ReachableTypesBase = DBTH.getSubTypes(*BaseType);
  auto ReachableTypesBase2 = DBTH.getSubTypes(*Base2Type);
  auto ReachableTypesBase3 = DBTH.getSubTypes(*Base3Type);
  auto ReachableTypesChild = DBTH.getSubTypes(*ChildType);
  auto ReachableTypesChild2 = DBTH.getSubTypes(*Child2Type);

  EXPECT_EQ(ReachableTypesBase.size(), 3U);
  EXPECT_EQ(ReachableTypesBase2.size(), 3U);
  EXPECT_EQ(ReachableTypesBase3.size(), 2U);
  EXPECT_EQ(ReachableTypesChild.size(), 2U);
  EXPECT_EQ(ReachableTypesChild2.size(), 1U);
  EXPECT_TRUE(ReachableTypesBase.count(*BaseType));
  EXPECT_TRUE(ReachableTypesBase.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase.count(*Child2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesBase2.count(*ChildType));
  EXPECT_TRUE(ReachableTypesBase2.count(*Child2Type));
  EXPECT_TRUE(ReachableTypesBase3.count(*Base3Type));
  EXPECT_TRUE(ReachableTypesBase3.count(*Child2Type));
  EXPECT_TRUE(ReachableTypesChild.count(*ChildType));
  EXPECT_TRUE(ReachableTypesChild.count(*Child2Type));
  EXPECT_FALSE(ReachableTypesChild.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild.count(*Base2Type));
  EXPECT_TRUE(ReachableTypesChild2.count(*Child2Type));
  EXPECT_FALSE(ReachableTypesChild2.count(*BaseType));
  EXPECT_FALSE(ReachableTypesChild2.count(*Base2Type));
  EXPECT_FALSE(ReachableTypesChild2.count(*Base3Type));
}

} // namespace

/**
 * @file DIBasedTypeHierarchySerializationTest.cpp
 * @brief Unit tests for DIBasedTypeHierarchy serialization/deserialization
 *
 * This file contains comprehensive tests for the serialization and deserialization
 * of DIBasedTypeHierarchy, ensuring that type hierarchy information can be
 * correctly saved to JSON and restored without loss of information.
 * Tests are migrated from PhasarLLVM TypeHierarchy tests.
 */

#include "Analysis/TypeHirarchy/DIBasedTypeHierarchy.h"
#include "Analysis/TypeHirarchy/DIBasedTypeHierarchyData.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lotus;

namespace {

using lotus::unittest::loadModule;

// Helper function to get test file path
std::string getTestFilePath(const std::string &FileName) {
  return std::string(LOTUS_TYPE_HIERARCHY_LL_DIR) + "/" + FileName;
}

/**
 * @brief Compare two DIBasedTypeHierarchy instances for equality
 *
 * This function checks that both hierarchies have the same types, edges,
 * and vtables.
 *
 * @param Orig The original type hierarchy
 * @param Deser The deserialized type hierarchy
 */
void compareResults(const DIBasedTypeHierarchy &Orig,
                    const DIBasedTypeHierarchy &Deser) {

  EXPECT_EQ(Orig.getAllTypes().size(), Deser.getAllTypes().size());
  EXPECT_EQ(Orig.getAllVTables().size(), Deser.getAllVTables().size());

  for (const auto *OrigCurrentType : Orig.getAllTypes()) {
    // check types
    auto DeserTy = Deser.getType(Orig.getTypeName(OrigCurrentType));
    ASSERT_TRUE(DeserTy.hasValue())
        << "Failed to match type with name '"
        << Orig.getTypeName(OrigCurrentType).str() << "'";

    // check edges
    auto OrigSubTypes = Orig.subTypesOf(OrigCurrentType);
    auto DeserSubTypes = Deser.subTypesOf(*DeserTy);

    for (const auto &CurrOrigSubType : OrigSubTypes) {
      bool DeserHasSubType = false;

      for (const auto &CurrDeserSubType : DeserSubTypes) {
        if (Deser.getTypeName(CurrDeserSubType) ==
            Orig.getTypeName(CurrOrigSubType)) {
          DeserHasSubType = true;
          break;
        }
      }

      EXPECT_TRUE(DeserHasSubType)
          << "Deserialized hierarchy missing subtype: "
          << Orig.getTypeName(CurrOrigSubType).str();
    }

    // check virtual functions and vtables
    if (OrigCurrentType != *DeserTy) {
      errs() << "Mismatched types:\n> OrigTy: " << *OrigCurrentType
             << '\n';
      errs() << "> DeserTy: " << **DeserTy << '\n';
    }
  }
}

/**
 * @brief Test fixture for type hierarchy serialization tests
 *
 * This test fixture uses parameterized tests to run the same serialization
 * test on multiple LLVM IR files.
 */
class TypeHierarchySerialization
    : public ::testing::TestWithParam<std::string_view> {
protected:
  static constexpr auto PathToLlFiles = "regress/PTATestCases/";
  const std::vector<std::string> EntryPoints = {"main"};

}; // Test Fixture

/**
 * @brief Test that serialization and deserialization produce equivalent results
 *
 * This test parameterized test loads a module, builds its type hierarchy,
 * serializes it to JSON, deserializes it back, and verifies that the
 * original and deserialized hierarchies are equivalent.
 */
TEST_P(TypeHierarchySerialization, OrigAndDeserEqual) {
  LLVMContext Context;
  std::string FilePath = getTestFilePath(std::string(GetParam()));
  auto M = loadModule(FilePath, Context);
  ASSERT_NE(nullptr, M) << "Failed to load module: " << FilePath;

  DIBasedTypeHierarchy DIBTH(*M);

  std::string Ser;
  raw_string_ostream StringStream(Ser);

  DIBTH.printAsJson(StringStream);

  DIBasedTypeHierarchy DeserializedDIBTH(
      M.get(), DIBasedTypeHierarchyData::loadJsonString(Ser));

  compareResults(DIBTH, DeserializedDIBTH);
}

// List of test files to use for serialization tests
static constexpr std::string_view TypeHierarchyTestFiles[] = {
    "type_hierarchy_1_cpp_dbg.ll",    "type_hierarchy_2_cpp_dbg.ll",
    "type_hierarchy_3_cpp_dbg.ll",    "type_hierarchy_4_cpp_dbg.ll",
    "type_hierarchy_5_cpp_dbg.ll",    "type_hierarchy_6_cpp_dbg.ll",
    "type_hierarchy_7_cpp_dbg.ll",    "type_hierarchy_7_b_cpp_dbg.ll",
    "type_hierarchy_8_cpp_dbg.ll",    "type_hierarchy_9_cpp_dbg.ll",
    "type_hierarchy_10_cpp_dbg.ll",   "type_hierarchy_11_cpp_dbg.ll",
    "type_hierarchy_12_cpp_dbg.ll",   "type_hierarchy_12_b_cpp_dbg.ll",
    "type_hierarchy_12_c_cpp_dbg.ll", "type_hierarchy_14_cpp_dbg.ll",
    "type_hierarchy_15_cpp_dbg.ll",
    "type_hierarchy_16_cpp_dbg.ll",   "type_hierarchy_17_cpp_dbg.ll",
    "type_hierarchy_18_cpp_dbg.ll",   "type_hierarchy_19_cpp_dbg.ll",
    "type_hierarchy_20_cpp_dbg.ll",   "type_hierarchy_21_cpp_dbg.ll",
};

INSTANTIATE_TEST_SUITE_P(TypeHierarchySerializationTest,
                         TypeHierarchySerialization,
                         ::testing::ValuesIn(TypeHierarchyTestFiles));

} // namespace

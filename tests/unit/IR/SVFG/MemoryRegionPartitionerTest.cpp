#include "IR/SVFG/MemoryRegionPartitioner.h"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace lotus::analysis;
using namespace lotus::unittest;

class MemoryRegionPartitionerTest : public LlvmModuleTest {};

TEST_F(MemoryRegionPartitionerTest, IntraDisjointMergesOnlyWithinFunction) {
  auto module = parseModule(R"(
    define void @first() { ret void }
    define void @second() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  const llvm::Function *first = module->getFunction("first");
  const llvm::Function *second = module->getFunction("second");

  MemoryRegionPartitioner partitioner(
      MemoryRegionPartitionStrategy::IntraDisjoint);
  partitioner.observe(first, {1, 2});
  partitioner.observe(first, {2, 3});
  partitioner.observe(second, {2, 4});
  partitioner.freeze();

  EXPECT_EQ(partitioner.canonicalize(first, {1}), SVFGNodeBS({1, 2, 3}));
  EXPECT_EQ(partitioner.canonicalize(first, {3}), SVFGNodeBS({1, 2, 3}));
  EXPECT_EQ(partitioner.canonicalize(second, {2}), SVFGNodeBS({2, 4}));
}

TEST_F(MemoryRegionPartitionerTest, InterDisjointMergesAcrossFunctions) {
  auto module = parseModule(R"(
    define void @first() { ret void }
    define void @second() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  const llvm::Function *first = module->getFunction("first");
  const llvm::Function *second = module->getFunction("second");

  MemoryRegionPartitioner partitioner(
      MemoryRegionPartitionStrategy::InterDisjoint);
  partitioner.observe(first, {1, 2});
  partitioner.observe(second, {2, 3});
  partitioner.freeze();

  const SVFGNodeBS expected{1, 2, 3};
  EXPECT_EQ(partitioner.canonicalize(first, {1}), expected);
  EXPECT_EQ(partitioner.canonicalize(second, {3}), expected);
  EXPECT_EQ(partitioner.statistics().regions, 1u);
}

TEST_F(MemoryRegionPartitionerTest, DistinctPreservesExactSets) {
  MemoryRegionPartitioner partitioner(MemoryRegionPartitionStrategy::Distinct);
  partitioner.observe(nullptr, {1, 2});
  partitioner.observe(nullptr, {2, 3});
  partitioner.freeze();

  EXPECT_EQ(partitioner.canonicalize(nullptr, {1, 2}), SVFGNodeBS({1, 2}));
  EXPECT_EQ(partitioner.canonicalize(nullptr, {2, 3}), SVFGNodeBS({2, 3}));
}

#include "Analysis/NullPointer/LocalNullCheckAnalysis.h"
#include "Analysis/NullPointer/NullFlowAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <climits>
#include <cstdint>
#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace lotus::unittest;

namespace lotus {
namespace nullpointer {
namespace testing {
void setNullFlowIncrementalLimitOverrideForTesting(int Limit);
unsigned getNullFlowIncrementalLimitForTesting();
bool isContextInsensitiveGuaranteedNonNullValueForTesting(llvm::Value *V);
bool allIncomingEdgesUnreachableForTesting(llvm::Function *F,
                                           llvm::Instruction *Inst,
                                           const std::set<Edge> &Unreachable);
} // namespace testing
} // namespace nullpointer
} // namespace lotus

namespace {

class ScopedLimitOverride {
public:
  explicit ScopedLimitOverride(int Limit) {
    lotus::nullpointer::testing::
        setNullFlowIncrementalLimitOverrideForTesting(Limit);
  }

  ~ScopedLimitOverride() {
    lotus::nullpointer::testing::
        setNullFlowIncrementalLimitOverrideForTesting(INT_MIN);
  }
};

TEST(NullPointerLegacyHelpersTest, ZeroLimitMeansUnlimited) {
  ScopedLimitOverride LimitOverride(0);
  EXPECT_EQ(lotus::nullpointer::testing::
                getNullFlowIncrementalLimitForTesting(),
            UINT32_MAX);
}

TEST(NullPointerLegacyHelpersTest,
     OnlyStackOrNonnullContractsAreGuaranteedNonNullLocally) {
  llvm::LLVMContext Context;
  auto Module = parseModule(Context, R"(
    declare i8* @malloc(i64)
    declare nonnull i8* @returns_nonnull()

    define void @example() {
    entry:
      %stack = alloca i8, align 1
      %heap = call i8* @malloc(i64 8)
      %contract = call i8* @returns_nonnull()
      ret void
    }
  )", "NullPointerLegacyHelpersTest");
  ASSERT_NE(Module, nullptr);

  auto *Function = Module->getFunction("example");
  ASSERT_NE(Function, nullptr);

  auto *Stack = findInstructionByName(Function, "stack");
  auto *Heap = findInstructionByName(Function, "heap");
  auto *Contract = findInstructionByName(Function, "contract");
  ASSERT_NE(Stack, nullptr);
  ASSERT_NE(Heap, nullptr);
  ASSERT_NE(Contract, nullptr);

  EXPECT_TRUE(lotus::nullpointer::testing::
                  isContextInsensitiveGuaranteedNonNullValueForTesting(Stack));
  EXPECT_FALSE(lotus::nullpointer::testing::
                   isContextInsensitiveGuaranteedNonNullValueForTesting(Heap));
  EXPECT_TRUE(
      lotus::nullpointer::testing::
          isContextInsensitiveGuaranteedNonNullValueForTesting(Contract));
}

TEST(NullPointerLegacyHelpersTest,
     EntryInstructionIsNotTreatedAsUnreachableWithoutPredecessors) {
  llvm::LLVMContext Context;
  auto Module = parseModule(Context, R"(
    define void @example(i8* %p) {
    entry:
      %cmp = icmp eq i8* %p, null
      br i1 %cmp, label %null, label %nonnull

    null:
      ret void

    nonnull:
      ret void
    }
  )", "NullPointerLegacyHelpersTest");
  ASSERT_NE(Module, nullptr);

  auto *Function = Module->getFunction("example");
  auto *Cmp = findInstructionByName(Function, "cmp");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Cmp, nullptr);

  std::set<Edge> Unreachable;
  EXPECT_FALSE(lotus::nullpointer::testing::
                   allIncomingEdgesUnreachableForTesting(Function, Cmp,
                                                         Unreachable));
}

} // namespace

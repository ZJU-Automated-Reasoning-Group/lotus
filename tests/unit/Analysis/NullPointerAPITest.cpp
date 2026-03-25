#include "Analysis/NullPointer/API.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace lotus::unittest;

namespace {

TEST(NullPointerAPITest, DistinguishesHeapAndStackAllocations) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i32 @example() {
    entry:
      %stack = alloca i32, align 4
      %heap = call i8* @malloc(i64 8)
      store i32 0, i32* %stack, align 4
      ret i32 0
    }
  )", "NullPointerAPITest");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *stack_alloc = findInstructionByName(function, "stack");
  auto *heap_alloc = findInstructionByName(function, "heap");
  ASSERT_NE(stack_alloc, nullptr);
  ASSERT_NE(heap_alloc, nullptr);

  EXPECT_TRUE(API::isStackAllocate(stack_alloc));
  EXPECT_FALSE(API::isHeapAllocate(stack_alloc));
  EXPECT_TRUE(API::isMemoryAllocate(stack_alloc));

  EXPECT_TRUE(API::isHeapAllocate(heap_alloc));
  EXPECT_FALSE(API::isStackAllocate(heap_alloc));
  EXPECT_TRUE(API::isMemoryAllocate(heap_alloc));
}

TEST(NullPointerAPITest, IgnoresNonAllocationCalls) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @free(i8*)

    define void @example(i8* %ptr) {
    entry:
      call void @free(i8* %ptr)
      ret void
    }
  )", "NullPointerAPITest");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *call = llvm::dyn_cast<llvm::CallInst>(&function->getEntryBlock().front());
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(API::isHeapAllocate(call));
  EXPECT_FALSE(API::isStackAllocate(call));
  EXPECT_FALSE(API::isMemoryAllocate(call));
}

} // namespace

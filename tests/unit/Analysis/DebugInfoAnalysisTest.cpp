#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::parseModule;

const llvm::Instruction *findInstruction(const llvm::Function *function,
                                         llvm::StringRef name) {
  for (const auto &bb : *function) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

TEST(DebugInfoAnalysisTest, FallsBackToIrNamesWithoutDebugMetadata) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example() {
    entry:
      %value = add i32 1, 2
      ret i32 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  const auto *value = findInstruction(function, "value");
  ASSERT_NE(value, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_EQ(analysis.getFunctionName(value), "example");
  EXPECT_EQ(analysis.getVariableName(value), "value");
  EXPECT_EQ(analysis.getTypeName(value), "i32");
  EXPECT_EQ(analysis.getSourceLocation(value), "unknown:0");
}

TEST(DebugInfoAnalysisTest, UsesInstructionNameWhenNoDebugVariableExists) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare i32 @producer()

    define i32 @example() {
    entry:
      %tmp = call i32 @producer()
      ret i32 %tmp
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  const auto *call = findInstruction(function, "tmp");
  ASSERT_NE(call, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_EQ(analysis.getVariableName(call), "tmp");
  EXPECT_EQ(analysis.getFunctionName(call), "example");
}

} // namespace

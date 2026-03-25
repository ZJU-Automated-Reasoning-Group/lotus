/*
 * Private implementation details for the CT-LLVM analysis pass.
 */

#pragma once

#define LLVM_ENABLE_DUMP

#include "Analysis/Crypto/ctllvm.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace ctllvm {
namespace detail {

struct TargetValueInfo {
  std::string function_name = "0";
  std::string value_name = "0";
  std::string value_type = "0";
  std::string field_name = "0";
  int line_number = -1;
};

enum class AnalysisError {
  None = 0,
  InlineAssembly = -1,
  IndirectCall = -2,
  NoImplementation = -3,
  InvokeFunction = -4,
  InlineItself = -5,
  InlineFail = -6,
  NotCallBase = -7,
  OverThreshold = -8,
  TooManyAlias = -9,
  NoConstantSize = -10,
};

struct FunctionAnalysisResult {
  int violation_mask = 0;
  AnalysisError error = AnalysisError::None;
};

struct InliningResult {
  llvm::Function *function = nullptr;
  AnalysisError error = AnalysisError::None;

  bool succeeded() const {
    return function != nullptr && error == AnalysisError::None;
  }
};

struct Statistics {
  std::vector<AnalysisError> cannot_inline_cases;
  int taint_source = 0;
  int secure_taint_source = 0;
  int analyzed_functions = 0;
  int too_many_alias = 0;
  int overall_functions = 0;
  int secure_functions = 0;
  int inline_success = 0;
  int inline_fail = 0;
  int no_constant_size = 0;
};

inline bool isError(AnalysisError error) {
  return error != AnalysisError::None;
}

inline bool hasNamePrefix(llvm::StringRef name, llvm::StringRef prefix) {
#if LLVM_VERSION_MAJOR > 15
  return name.starts_with(prefix);
#else
  return name.startswith(prefix);
#endif
}

inline bool hasNameSuffix(llvm::StringRef name, llvm::StringRef suffix) {
#if LLVM_VERSION_MAJOR > 15
  return name.ends_with(suffix);
#else
  return name.endswith(suffix);
#endif
}

inline bool hasPointerLikeType(const llvm::Type *type) {
  return type->isPointerTy() || type->isArrayTy() || type->isStructTy();
}

inline llvm::StringRef clonedFunctionSuffix() { return "_ctcloned"; }

inline llvm::StringRef stripClonedSuffix(llvm::StringRef name) {
  if (hasNameSuffix(name, clonedFunctionSuffix())) {
    return name.drop_back(clonedFunctionSuffix().size());
  }
  return name;
}

llvm::StringRef getDebugName(llvm::Value *V, llvm::StringRef Name,
                             llvm::Function &F);
llvm::Value *getDebugValue(llvm::Value *V, llvm::StringRef Name,
                           llvm::Function &F);
int getDebugLine(llvm::Value *V, llvm::StringRef Name, llvm::Function &F);

class CryptoAnalysisImpl {
public:
  explicit CryptoAnalysisImpl(const CTOptions &options);

  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

  FunctionAnalysisResult analyzeFunction(llvm::Function &F,
                                         llvm::FunctionAnalysisManager &FAM);
  void defUseOnly(llvm::SetVector<llvm::Instruction *> &taintedInstructions,
                  llvm::SetVector<llvm::Value *> &declassified_values);
  void defUseAlias(llvm::SetVector<llvm::Instruction *> &taintedInstructions,
                   llvm::SetVector<llvm::Instruction *> &aliasedInstructions,
                   llvm::SetVector<llvm::Instruction *> &memoryInstructions,
                   llvm::AAResults &AA, llvm::Value *Arg,
                   llvm::SetVector<llvm::Value *> &declassified_values);
  void
  defUseMayAlias(llvm::SetVector<llvm::Instruction *> &taintedInstructions,
                 llvm::SetVector<llvm::Instruction *> &aliasedInstructions,
                 llvm::SetVector<llvm::Instruction *> &memoryInstructions,
                 llvm::AAResults &AA, llvm::Value *Arg,
                 llvm::SetVector<llvm::Value *> &declassified_values);
  int buildDependencyChain(
      llvm::SetVector<llvm::Instruction *> &taintedInstructions,
      llvm::SetVector<llvm::Value *> &declassified_values);
  int findAliasedInstructions(
      llvm::SetVector<llvm::Instruction *> &aliasedInstructions,
      llvm::SetVector<llvm::Instruction *> &taintedInstructions,
      llvm::SetVector<llvm::Instruction *> &memoryInstructions,
      llvm::AAResults &AA, llvm::Value *Arg,
      llvm::SetVector<llvm::Value *> &declassified_values);
  bool reasonMemcpy(llvm::Instruction &I, llvm::AliasAnalysis &AA,
                    llvm::SetVector<llvm::Instruction *> &memoryInstructions);
  bool wrapMetadata(llvm::Instruction &I, llvm::Value *Arg, bool alias_flag,
                    bool init_flag = false,
                    llvm::Value *initial_taint_arg = nullptr);

  bool updateTargetValues(std::vector<TargetValueInfo> &target_values,
                          std::vector<TargetValueInfo> &declassified_values);
  bool updateTaintList(llvm::Module &M, llvm::Function &F,
                       llvm::Instruction &I,
                       bool declassify_flag,
                       llvm::SetVector<llvm::Value *> &tainted_values,
                       llvm::ArrayRef<TargetValueInfo> entries);
  int getFieldIndex(llvm::StructType *StructTy, llvm::StringRef FieldName,
                    const llvm::Module &M);

  void checkInstructionLeaks(
      llvm::SetVector<llvm::Instruction *> &taintedInstructions,
      std::map<llvm::Instruction *, int> &leak_through_cache,
      std::map<llvm::Instruction *, int> &leak_through_branch,
      std::map<llvm::Instruction *, int> &leak_through_variable_timing,
      llvm::Value *Arg, llvm::Function &F,
      llvm::FunctionAnalysisManager &FAM);
  int checkAndReport(
      llvm::Value *Arg, llvm::Function &F, llvm::FunctionAnalysisManager &FAM,
      llvm::SetVector<llvm::Instruction *> &taintedInstructions,
      std::map<llvm::Instruction *, int> &leak_through_cache,
      std::map<llvm::Instruction *, int> &leak_through_branch,
      std::map<llvm::Instruction *, int> &leak_through_variable_timing,
      int mode);
  void printLeakage(
      const std::string &type,
      const std::map<llvm::Instruction *, int> &leakMap, int may_must,
      llvm::SetVector<llvm::Instruction *> &taintedInstructions);
  void
  reportLeakage(llvm::SetVector<llvm::Instruction *> &taintedInstructions,
                std::map<llvm::Instruction *, int> &leak_through_cache,
                std::map<llvm::Instruction *, int> &leak_through_branch,
                std::map<llvm::Instruction *, int> &leak_through_variable_timing,
                int may_must);
  void printSourceCode(const std::string &filename, int line_number);
  void printStatistics();

  AnalysisError getFunctionCalls(llvm::Function &F,
                                 std::set<llvm::Function *> &functions_to_inline,
                                 unsigned &count);
  AnalysisError
  inlineFunctionCalls(llvm::Function &F,
                      std::set<llvm::Function *> &functions_to_inline,
                      unsigned &count);
  InliningResult recursiveInlineCalls(llvm::Function *targetFunction);
  void updateSecureFunctionNames();

private:
  const CTOptions options_;
  Statistics statistics_;
  llvm::StringSet<> secure_function_names_;
  std::vector<TargetValueInfo> specified_target_values_;
  std::vector<TargetValueInfo> specified_declassified_values_;
  bool specify_taint_flag_ = false;

  llvm::SetVector<llvm::Value *> high_values_;
  llvm::SetVector<llvm::Value *> low_values_;
  llvm::SetVector<llvm::Value *> high_mayvalues_;
  llvm::SetVector<llvm::Value *> low_mayvalues_;
};

} // namespace detail
} // namespace ctllvm

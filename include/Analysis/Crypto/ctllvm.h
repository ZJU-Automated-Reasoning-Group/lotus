/**
 * @file ctllvm.h
 * @brief Public interface for the CT-LLVM constant-time analysis pass.
 */

#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassPlugin.h"

#include <string>

/**
 * @brief Runtime configuration for the CT-LLVM analysis pass.
 *
 * The default values intentionally match the legacy macro-based behavior.
 */
struct CTOptions {
  std::string file_path;
  bool type_system = true;
  bool test_all_parameters = true;
  bool enable_may_leak = true;
  bool try_hard_on_name = true;
  bool user_specify = false;
  bool soundness_mode = true;
  int alias_threshold = 2000;
  bool report_leakages = true;
  bool time_analysis = false;
  bool auto_continue = true;
  int inline_threshold = 10;
  bool debug = false;
  bool print_function = false;
};

/**
 * @brief Constant-time analysis pass for side-channel detection.
 */
struct CTPass : public llvm::PassInfoMixin<CTPass> {
  CTPass();
  explicit CTPass(CTOptions options);

  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM);

private:
  CTOptions options_;
};

llvm::PassPluginLibraryInfo getPassPluginInfo();

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo();

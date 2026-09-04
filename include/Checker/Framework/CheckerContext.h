/** @file CheckerContext.h @brief Per-module checker context providing analysis-wide dependencies. */
#pragma once

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/IR/Module.h>

namespace lotus::checker {

struct CheckerContext {
  llvm::Module &module;
  lotus::AliasAnalysisWrapper *alias_analysis = nullptr;
};

} // namespace lotus::checker

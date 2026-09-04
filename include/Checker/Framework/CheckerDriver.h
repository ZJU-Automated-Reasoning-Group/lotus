/** @file CheckerDriver.h @brief Checker driver that orchestrates analysis execution and reporting. */
#pragma once

#include "Checker/Framework/CheckerContext.h"
#include "Checker/Framework/CheckerDiagnostic.h"
#include "Checker/Framework/CheckerRegistry.h"

#include <llvm/Support/Error.h>

#include <vector>

namespace lotus::checker {

class CheckerDriver {
public:
  CheckerDriver(const CheckerRegistry &registry, CheckerContext &context)
      : registry_(registry), context_(context) {}

  llvm::Expected<std::vector<CheckerDiagnostic>>
  run(const std::vector<const CheckerDescriptor *> &selection) const;

  llvm::Error
  emitToReportManager(const std::vector<CheckerDiagnostic> &diagnostics) const;

private:
  const CheckerRegistry &registry_;
  CheckerContext &context_;
};

} // namespace lotus::checker

/** @file CheckerValidator.h @brief Validation utilities for checker specifications and configurations. */
#pragma once

#include "Checker/Framework/CheckerTypes.h"

#include <llvm/Support/Error.h>

#include <string>
#include <unordered_set>

namespace lotus::checker {

class CheckerValidator {
public:
  static llvm::Error validate(const CheckerSpec &spec,
                              const std::unordered_set<std::string> &existing_ids = {});
};

} // namespace lotus::checker

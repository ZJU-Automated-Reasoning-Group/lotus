/** @file CheckerSpecLoader.h @brief Loader for declarative checker specifications from YAML/JSON. */
#pragma once

#include "Checker/Framework/CheckerTypes.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

#include <vector>

namespace lotus::checker {

class CheckerSpecLoader {
public:
  llvm::Expected<CheckerSpec> loadFromBuffer(llvm::StringRef yaml,
                                             llvm::StringRef source_name) const;
  llvm::Expected<std::vector<CheckerSpec>>
  loadFromDirectory(llvm::StringRef directory) const;
};

} // namespace lotus::checker

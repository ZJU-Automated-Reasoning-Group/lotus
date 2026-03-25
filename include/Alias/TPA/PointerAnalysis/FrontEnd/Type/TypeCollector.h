#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeSet.h"

namespace llvm {
class Module;
} // namespace llvm

namespace tpa {

class TypeCollector {
public:
  TypeCollector() = default;

  TypeSet runOnModule(const llvm::Module &);
};

} // namespace tpa

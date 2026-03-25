#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"

namespace llvm {
class Module;
} // namespace llvm

namespace tpa {

class TypeAnalysis {
public:
  TypeAnalysis() = default;

  TypeMap runOnModule(const llvm::Module &);
};

} // namespace tpa
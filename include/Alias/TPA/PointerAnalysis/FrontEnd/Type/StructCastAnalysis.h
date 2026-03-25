#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/CastMap.h"

namespace llvm {
class Module;
} // namespace llvm

namespace tpa {

class StructCastAnalysis {
public:
  CastMap runOnModule(const llvm::Module &);
};

} // namespace tpa

#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace tpa {

class SemiSparseProgramBuilder {
private:
  void buildCFGForFunction(tpa::SemiSparseProgram &, const llvm::Function &,
                           const TypeMap &);

public:
  tpa::SemiSparseProgram runOnModule(const llvm::Module &module);
};

} // namespace tpa

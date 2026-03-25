#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace tpa {

class CFG;
class CFGNode;

class CFGBuilder {
private:
  CFG &cfg;
  const TypeMap &typeMap;

  CFGNode *translateInstruction(const llvm::Instruction &);

public:
  CFGBuilder(CFG &c, const TypeMap &t);

  void buildCFG(const llvm::Function &);
};

} // namespace tpa

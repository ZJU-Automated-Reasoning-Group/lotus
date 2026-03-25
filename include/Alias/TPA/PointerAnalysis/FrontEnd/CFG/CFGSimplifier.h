#pragma once

#include "Alias/TPA/Util/DataStructure/VectorSet.h"

#include <llvm/ADT/DenseMap.h>

namespace llvm {
class Value;
} // namespace llvm

namespace tpa {

class CFG;
class CFGNode;

class CFGSimplifier {
private:
  llvm::DenseMap<const llvm::Value *, const llvm::Value *> eqMap;

  std::vector<CFGNode *> findRedundantNodes(CFG &);
  void flattenEquivalentMap();
  void adjustCFG(CFG &, const util::VectorSet<CFGNode *> &);
  void adjustDefUseChain(CFG &, const util::VectorSet<CFGNode *> &);
  void removeNodes(CFG &, const util::VectorSet<CFGNode *> &);

public:
  CFGSimplifier() {}

  void simplify(CFG &);
};

} // namespace tpa
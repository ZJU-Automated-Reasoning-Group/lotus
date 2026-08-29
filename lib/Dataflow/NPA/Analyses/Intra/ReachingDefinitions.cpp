/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Intra/ReachingDefinitions.h"

#include "Dataflow/NPA/LLVM/BitVectorProblem.h"

#include <unordered_map>

#include <llvm/IR/Instructions.h>

namespace npa {

class RDInfo : public BitVectorProblem {
  unsigned bitWidth;
  std::unordered_map<const llvm::Value *, unsigned> valToBit;
  std::unordered_map<const llvm::BasicBlock *, llvm::APInt> genMap;
  std::unordered_map<const llvm::BasicBlock *, llvm::APInt> killMap;

public:
  RDInfo(llvm::Function &F) {
    // 1. Map Definitions to Bits
    unsigned bit = 0;
    for (auto &Arg : F.args()) {
      valToBit[&Arg] = bit++;
    }
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!I.getType()->isVoidTy()) {
          valToBit[&I] = bit++;
        }
      }
    }
    bitWidth = bit;
    if (bitWidth == 0)
      bitWidth = 1; // Handle empty function case

    // 2. Compute GEN/KILL Sets
    for (auto &BB : F) {
      llvm::APInt gen(bitWidth, 0);
      llvm::APInt kill(bitWidth, 0);

      // For Reaching Definitions on SSA:
      // GEN = All definitions in the block
      // KILL = Empty (SSA values are never redefined)

      for (auto &I : BB) {
        if (valToBit.count(&I)) {
          gen.setBit(valToBit[&I]);
        }
      }

      genMap[&BB] = gen;
      killMap[&BB] = kill;
    }
  }

  unsigned getBitWidth() const override { return bitWidth; }

  llvm::APInt getGen(const llvm::BasicBlock *BB) const override {
    if (genMap.count(BB))
      return genMap.at(BB);
    return llvm::APInt(bitWidth, 0);
  }

  llvm::APInt getKill(const llvm::BasicBlock *BB) const override {
    if (killMap.count(BB))
      return killMap.at(BB);
    return llvm::APInt(bitWidth, 0);
  }

  bool isForward() const override { return true; }
};

BitVectorSolver::Result
ReachingDefinitions::run(llvm::Function &F, SolverStrategy strategy,
                         LinearStrategy linearStrategy) {
  RDInfo info(F);
  return BitVectorSolver::run(F, info, strategy, linearStrategy);
}

} // namespace npa

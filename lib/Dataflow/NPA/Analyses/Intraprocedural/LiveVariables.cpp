/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Intraprocedural/LiveVariables.h"

#include "Dataflow/NPA/Domains/BitVectorInfo.h"

#include <unordered_map>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>

namespace npa {

namespace {

class LiveVariablesInfo : public BitVectorInfo {
public:
  explicit LiveVariablesInfo(llvm::Function &F) {
    unsigned bit = 0;
    for (auto &Arg : F.args())
      valueToBit[&Arg] = bit++;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!I.getType()->isVoidTy())
          valueToBit[&I] = bit++;
      }
    }
    bitWidth = bit == 0 ? 1 : bit;

    for (auto &BB : F) {
      llvm::APInt gen(bitWidth, 0);
      llvm::APInt kill(bitWidth, 0);

      for (auto It = BB.rbegin(); It != BB.rend(); ++It) {
        llvm::Instruction &I = *It;

        if (!I.getType()->isVoidTy()) {
          auto DefIt = valueToBit.find(&I);
          if (DefIt != valueToBit.end()) {
            gen.clearBit(DefIt->second);
            kill.setBit(DefIt->second);
          }
        }

        for (llvm::Use &Op : I.operands()) {
          llvm::Value *V = Op.get();
          auto UseIt = valueToBit.find(V);
          if (UseIt != valueToBit.end() && !kill[UseIt->second])
            gen.setBit(UseIt->second);
        }
      }

      genMap[&BB] = gen;
      killMap[&BB] = kill;
    }
  }

  unsigned getBitWidth() const override { return bitWidth; }

  llvm::APInt getGen(const llvm::BasicBlock *BB) const override {
    auto It = genMap.find(BB);
    if (It != genMap.end())
      return It->second;
    return llvm::APInt(bitWidth, 0);
  }

  llvm::APInt getKill(const llvm::BasicBlock *BB) const override {
    auto It = killMap.find(BB);
    if (It != killMap.end())
      return It->second;
    return llvm::APInt(bitWidth, 0);
  }

  bool isForward() const override { return false; }

private:
  unsigned bitWidth = 1;
  std::unordered_map<const llvm::Value *, unsigned> valueToBit;
  std::unordered_map<const llvm::BasicBlock *, llvm::APInt> genMap;
  std::unordered_map<const llvm::BasicBlock *, llvm::APInt> killMap;
};

} // namespace

BitVectorSolver::Result LiveVariables::run(llvm::Function &F,
                                           SolverStrategy strategy) {
  LiveVariablesInfo info(F);
  return BitVectorSolver::run(F, info, strategy);
}

} // namespace npa

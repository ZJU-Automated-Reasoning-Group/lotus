/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/LiveVariables.h"

#include "Dataflow/NPA/LLVM/BackwardInterEngine.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

namespace {

class LiveVariablesInfo {
public:
  explicit LiveVariablesInfo(llvm::Module &M) { buildUniverse(M); }

  unsigned getBitWidth() const { return nextBit == 0 ? 1u : nextBit; }

  unsigned getBit(const llvm::Value *V) const {
    auto It = valueBits.find(V);
    if (It == valueBits.end())
      return invalidBit();
    return It->second;
  }

  const std::unordered_map<const llvm::Value *, unsigned> &getBits() const {
    return valueBits;
  }

  static unsigned invalidBit() { return static_cast<unsigned>(-1); }

private:
  unsigned nextBit = 0;
  std::unordered_map<const llvm::Value *, unsigned> valueBits;

  void addValue(const llvm::Value *V) {
    if (!V || valueBits.count(V))
      return;
    const llvm::Type *Ty = V->getType();
    if (!Ty || Ty->isVoidTy())
      return;
    valueBits.emplace(V, nextBit++);
  }

  void buildUniverse(llvm::Module &M) {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      for (auto &Arg : F.args())
        addValue(&Arg);
      for (auto &BB : F) {
        for (auto &I : BB)
          addValue(&I);
      }
    }
  }
};

class InterproceduralLiveAnalysis {
public:
  using FactType = llvm::APInt;
  using D = TaintTransformer;
  using Exp = Exp0<D>;
  using E = E0<D>;

  explicit InterproceduralLiveAnalysis(llvm::Module &M)
      : info(M), bitWidth(info.getBitWidth()), widthScope(bitWidth) {}

  FactType getExitValue(const llvm::Function &) const {
    return llvm::APInt(bitWidth, 0);
  }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    D::value_type transfer = D::one();
    bool updated = false;

    if (!I.getType()->isVoidTy()) {
      unsigned defBit = info.getBit(&I);
      if (defBit != LiveVariablesInfo::invalidBit()) {
        clearInput(transfer, defBit);
        updated = true;
      }
    }

    if (llvm::isa<llvm::CallBase>(&I)) {
      if (!updated)
        return currentPath;
      return Exp::seq(transfer, currentPath);
    }

    for (const llvm::Use &Use : I.operands()) {
      const llvm::Value *V = Use.get();
      unsigned useBit = info.getBit(V);
      if (useBit != LiveVariablesInfo::invalidBit()) {
        D::addGen(transfer, useBit);
        updated = true;
      }
    }

    if (!updated)
      return currentPath;
    return Exp::seq(transfer, currentPath);
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &Call,
                                      const llvm::Function &Callee) {
    D::value_type transfer = D::one();
    if (!Call.getType()->isVoidTy()) {
      unsigned resultBit = info.getBit(&Call);
      if (resultBit != LiveVariablesInfo::invalidBit()) {
        clearInput(transfer, resultBit);
        for (const auto &BB : Callee) {
          auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
          if (!Ret)
            continue;
          unsigned retBit = info.getBit(Ret->getReturnValue());
          if (retBit != LiveVariablesInfo::invalidBit())
            D::addEdge(transfer, resultBit, retBit);
        }
      }
    }
    return transfer;
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &Call,
                                     const llvm::Function &Callee) {
    D::value_type transfer = D::one();
    const auto *ParamIt = Callee.arg_begin();
    for (unsigned i = 0; i < Call.arg_size() && ParamIt != Callee.arg_end();
         ++i, ++ParamIt) {
      unsigned paramBit = info.getBit(&*ParamIt);
      if (paramBit == LiveVariablesInfo::invalidBit())
        continue;
      clearInput(transfer, paramBit);
      unsigned argBit = info.getBit(Call.getArgOperand(i));
      if (argBit != LiveVariablesInfo::invalidBit())
        D::addEdge(transfer, paramBit, argBit);
    }
    return transfer;
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    D::value_type transfer = D::one();
    if (!Call.getType()->isVoidTy()) {
      unsigned resultBit = info.getBit(&Call);
      if (resultBit != LiveVariablesInfo::invalidBit())
        clearInput(transfer, resultBit);
    }
    for (const llvm::Use &Use : Call.args()) {
      unsigned argBit = info.getBit(Use.get());
      if (argBit != LiveVariablesInfo::invalidBit())
        D::addGen(transfer, argBit);
    }
    return transfer;
  }

  FactType applySummary(const D::value_type &summary,
                        const FactType &fact) const {
    return D::apply(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return lhs | rhs;
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return lhs == rhs;
  }

  const LiveVariablesInfo &getInfo() const { return info; }

private:
  LiveVariablesInfo info;
  unsigned bitWidth = 1;
  D::WidthScope widthScope;

  void clearInput(D::value_type &transfer, unsigned inputBit) const {
    if (inputBit == LiveVariablesInfo::invalidBit() ||
        inputBit >= transfer.rel.size())
      return;
    transfer.rel[inputBit] = llvm::APInt(bitWidth, 0);
  }
};

} // namespace

InterLiveVariables::Result
InterLiveVariables::run(llvm::Module &M, bool verbose,
                        LinearStrategy linearStrategy,
                        IndirectCallResolutionMode callResolutionMode) {
  InterproceduralLiveAnalysis analysis(M);
  auto engineResult =
      BackwardInterEngine<TaintTransformer,
                          InterproceduralLiveAnalysis>::run(M, analysis,
                                                            verbose,
                                                            linearStrategy,
                                                            callResolutionMode);

  Result result;
  result.status = engineResult.status;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  result.valueBits = analysis.getInfo().getBits();
  result.bitWidth = analysis.getInfo().getBitWidth();
  return result;
}

} // namespace npa

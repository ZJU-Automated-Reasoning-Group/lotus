/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralRD.h"

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <llvm/IR/Instructions.h>

namespace npa {

// Analysis Policy for Reaching Definitions (Gen/Kill)
class RDAnalysis {
public:
  using FactType = llvm::APInt; // Fact type for Phase 2

private:
  using D = GenKillTransferDomain;
  using Exp = Exp0<D>;
  using E = E0<D>;

  std::unordered_map<const llvm::Value *, unsigned> valToBit;
  unsigned bitWidth;
  D::WidthScope widthScope;

public:
  RDAnalysis(llvm::Module &M) {
    unsigned bit = 0;
    for (auto &F : M) {
      for (auto &Arg : F.args())
        valToBit[&Arg] = bit++;
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (!I.getType()->isVoidTy())
            valToBit[&I] = bit++;
        }
      }
    }
    bitWidth = (bit == 0) ? 1 : bit;
    widthScope.reset(bitWidth);
  }

  FactType getEntryValue() const {
    return llvm::APInt(bitWidth, 0); // Empty set of definitions
  }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (valToBit.count(&I)) {
      unsigned b = valToBit[&I];
      llvm::APInt gen(bitWidth, 0);
      gen.setBit(b);
      llvm::APInt kill(bitWidth, 0); // SSA: No kill of other defs
      return Exp::seq({kill, gen}, currentPath);
    }
    return currentPath;
  }

  // Phase 2: Application of Summary (Function) to Fact (APInt)
  // Summary is (Kill, Gen)
  // Fact is APInt
  // Result = (Fact \ Kill) U Gen
  llvm::APInt applySummary(const D::value_type &summary,
                           const llvm::APInt &fact) {
    return (fact & ~summary.first) | summary.second;
  }

  llvm::APInt joinFacts(const llvm::APInt &a, const llvm::APInt &b) {
    return a | b;
  }

  bool factsEqual(const llvm::APInt &a, const llvm::APInt &b) { return a == b; }
};

InterproceduralRD::Result
InterproceduralRD::run(llvm::Module &M, bool verbose,
                       LinearStrategy linearStrategy,
                       IndirectCallResolutionMode callResolutionMode) {
  RDAnalysis analysis(M);
  auto engineResult =
      InterproceduralEngine<GenKillTransferDomain, RDAnalysis>::run(
          M, analysis, verbose, linearStrategy, callResolutionMode);

  InterproceduralRD::Result res;
  res.status = engineResult.status;
  res.summaries.insert(engineResult.summaries.begin(),
                       engineResult.summaries.end());
  for (auto &kv : engineResult.blockEntryFacts) {
    res.blockFacts[kv.first] = kv.second;
  }
  return res;
}

} // namespace npa

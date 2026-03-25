#ifndef TCFS_ANDERSEN_AA_H
#define TCFS_ANDERSEN_AA_H

#include "Alias/SparrowAA/Andersen.h"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/PassManager.h>

class AndersenAAResult : public llvm::AAResultBase<AndersenAAResult> {
private:
  friend llvm::AAResultBase<AndersenAAResult>;

  Andersen anders;
  llvm::AliasResult andersenAlias(const llvm::Value *, const llvm::Value *);

public:
  /// Construct using the globally selected context policy (via -andersen-k-cs).
  AndersenAAResult(const llvm::Module &);
  /// Construct with an explicit context policy (e.g., NoCtx / 1-CFA / 2-CFA).
  AndersenAAResult(const llvm::Module &, ContextPolicy policy);
  /// Convenience ctor: choose k-call-site context sensitivity (0/1/2).
  AndersenAAResult(const llvm::Module &, unsigned kCallSite);

  llvm::AliasResult alias(const llvm::MemoryLocation &,
                          const llvm::MemoryLocation &);
  bool pointsToConstantMemory(const llvm::MemoryLocation &, bool);

  // Public method to access points-to information
  bool getPointsToSet(const llvm::Value *Ptr,
                      std::vector<const llvm::Value *> &PtsSet) const {
    return anders.getPointsToSet(Ptr, PtsSet);
  }

  // Context-sensitive points-to queries (no cross-context union).
  bool getPointsToSetInContext(const llvm::Value *Ptr,
                               AndersNodeFactory::CtxKey Ctx,
                               std::vector<const llvm::Value *> &PtsSet) const {
    return anders.getPointsToSetInContext(Ptr, Ctx, PtsSet);
  }

  // Context utilities for clients that want per-context answers.
  AndersNodeFactory::CtxKey getInitialContext() const {
    return anders.getInitialContext();
  }
  AndersNodeFactory::CtxKey getGlobalContext() const {
    return anders.getGlobalContext();
  }
  AndersNodeFactory::CtxKey evolveContext(AndersNodeFactory::CtxKey Prev,
                                          const llvm::Instruction *I) const {
    return anders.evolveContext(Prev, I);
  }
  std::string contextToString(AndersNodeFactory::CtxKey Ctx,
                              bool Detailed = false) const {
    return anders.contextToString(Ctx, Detailed);
  }
};

// New Pass Manager version
class AndersenAA : public llvm::AnalysisInfoMixin<AndersenAA> {
  friend llvm::AnalysisInfoMixin<AndersenAA>;
  static llvm::AnalysisKey Key;

public:
  using Result = AndersenAAResult;

  AndersenAAResult run(llvm::Module &M, llvm::ModuleAnalysisManager &MAM);
};

#endif

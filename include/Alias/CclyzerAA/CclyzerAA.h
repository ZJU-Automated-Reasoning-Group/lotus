/// Optional cclyzer++ (Datalog-based) alias analysis backend for Lotus.
/// Built only when LOTUS_USE_CCLYZER=ON and CCLYZERPP_ROOT is set.
/// Not integrated into AliasAnalysisWrapper; use this API directly.
#pragma once

#include <memory>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Value.h>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace cclyzer {

/// Standalone wrapper around cclyzer++ pointer analysis.
/// Run analysis on a module, then query alias and points-to.
class CclyzerAA {
public:
  CclyzerAA();
  ~CclyzerAA();

  /// Run Datalog-based pointer analysis on \p M.
  /// Uses cclyzerpp defaults (subset analysis, context sensitivity from
  /// process command line or defaults). Returns true if analysis succeeded.
  bool run(llvm::Module &M);

  /// Query alias between two pointers. Call after run().
  llvm::AliasResult alias(const llvm::Value *v1, const llvm::Value *v2);

  /// Query alias between two memory locations.
  llvm::AliasResult alias(const llvm::MemoryLocation &loc1,
                          const llvm::MemoryLocation &loc2);

  /// Get points-to set for pointer \p ptr. Call after run().
  /// Returns true if the backend supports points-to and \p ptsSet was filled.
  bool getPointsToSet(const llvm::Value *ptr,
                      std::vector<const llvm::Value *> &ptsSet);

  /// Whether run() completed successfully and queries are valid.
  bool isInitialized() const { return _initialized; }

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  bool _initialized = false;
};

} // namespace cclyzer
} // namespace lotus

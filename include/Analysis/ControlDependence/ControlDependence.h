//===- ControlDependence.h - Control-dependence variants -------*- C++ -*-===//
//
// Part of the Lotus project.
//
// Provides standard control dependence (SCD), non-termination-sensitive
// control dependence (NTSCD), decisive-order dependence (DOD), their historic
// algorithmic variants, and strong control closure.
//
// The NTSCD, DOD, and strong-closure algorithms are derived from dg by Marek
// Chalupa, distributed under the MIT license. See the repository LICENSE.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <memory>

namespace llvm {
class BasicBlock;
class Function;
} // namespace llvm

namespace lotus::cd {

/// Algorithms supported by the control-dependence analysis.
enum class Algorithm {
  Standard,
  SCD = Standard,
  NTSCD,
  NTSCD2,
  NTSCDLegacy,
  NTSCDRanganath,
  NTSCDRanganathOriginal,
  DOD,
  DODRanganath,
  DODNTSCD,
  StrongControlClosure,
};

struct ControlDependenceOptions {
  Algorithm algorithm{Algorithm::Standard};
};

/// Block-level control-dependence results for one LLVM function.
///
/// A dependency P in getDependencies(B) means that execution of B is control
/// dependent on the terminator of P. getDependents(P) provides the inverse
/// relation. StrongControlClosure is closure-based and therefore queried with
/// getClosure(); it intentionally has no binary dependence relation.
class ControlDependenceAnalysis {
public:
  using BlockVector = llvm::SmallVector<const llvm::BasicBlock *, 4>;

  explicit ControlDependenceAnalysis(
      llvm::Function &function,
      ControlDependenceOptions options = ControlDependenceOptions());
  ~ControlDependenceAnalysis();

  ControlDependenceAnalysis(ControlDependenceAnalysis &&) noexcept;
  ControlDependenceAnalysis &operator=(ControlDependenceAnalysis &&) noexcept;

  ControlDependenceAnalysis(const ControlDependenceAnalysis &) = delete;
  ControlDependenceAnalysis &
  operator=(const ControlDependenceAnalysis &) = delete;

  llvm::Function &getFunction() const;
  Algorithm getAlgorithm() const;

  /// Blocks whose terminators control whether \p block executes.
  llvm::ArrayRef<const llvm::BasicBlock *>
  getDependencies(const llvm::BasicBlock *block) const;

  /// Blocks whose execution is controlled by \p block's terminator.
  llvm::ArrayRef<const llvm::BasicBlock *>
  getDependents(const llvm::BasicBlock *block) const;

  bool dependsOn(const llvm::BasicBlock *block,
                 const llvm::BasicBlock *predicate) const;

  /// Compute the strong control closure of \p blocks.
  ///
  /// This query is valid only for Algorithm::StrongControlClosure. Duplicate
  /// and foreign blocks are ignored. The returned order follows function
  /// order, making results stable across runs.
  BlockVector getClosure(llvm::ArrayRef<const llvm::BasicBlock *> blocks) const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace lotus::cd

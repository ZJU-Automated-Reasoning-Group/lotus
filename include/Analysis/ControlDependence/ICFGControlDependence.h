//===- ICFGControlDependence.h - Whole-ICFG adapter ------------*- C++ -*-===//

#pragma once

#include "Analysis/ControlDependence/ControlDependence.h"

class ICFG;
class ICFGNode;

namespace lotus::cd {

/// Runs the graph-based control-dependence variants over a Lotus ICFG.
///
/// Standard CD is intentionally unavailable here because it is defined using
/// a function post-dominator tree. NTSCD, DOD, their variants, and strong
/// closure operate directly on all nodes and edges present in the supplied
/// ICFG. The ICFG must outlive this analysis object.
class ICFGControlDependenceAnalysis {
public:
  using NodeVector = llvm::SmallVector<const ICFGNode *, 4>;

  explicit ICFGControlDependenceAnalysis(const ICFG &icfg,
                                         ControlDependenceOptions options = {
                                             Algorithm::NTSCD});
  ~ICFGControlDependenceAnalysis();

  ICFGControlDependenceAnalysis(ICFGControlDependenceAnalysis &&) noexcept;
  ICFGControlDependenceAnalysis &
  operator=(ICFGControlDependenceAnalysis &&) noexcept;

  ICFGControlDependenceAnalysis(const ICFGControlDependenceAnalysis &) = delete;
  ICFGControlDependenceAnalysis &
  operator=(const ICFGControlDependenceAnalysis &) = delete;

  Algorithm getAlgorithm() const;
  llvm::ArrayRef<const ICFGNode *> getDependencies(const ICFGNode *node) const;
  llvm::ArrayRef<const ICFGNode *> getDependents(const ICFGNode *node) const;
  bool dependsOn(const ICFGNode *node, const ICFGNode *predicate) const;
  NodeVector getClosure(llvm::ArrayRef<const ICFGNode *> nodes) const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace lotus::cd

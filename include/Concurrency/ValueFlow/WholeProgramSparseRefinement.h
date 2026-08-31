/**
 * @file WholeProgramSparseRefinement.h
 * @brief Ownership wrapper for whole-program thread-aware sparse refinement.
 */
#pragma once

#include "Concurrency/ValueFlow/SparseFlowSensitivePTA.h"
#include "Concurrency/ValueFlow/ThreadAwareSVFG.h"
#include "IR/ICFG/ICFG.h"

#include <memory>

namespace llvm {
class Module;
} // namespace llvm

namespace mhp {
class IMHPAnalysis;
class LockSetAnalysis;
} // namespace mhp

namespace lotus::analysis {

class WholeProgramSparseRefinement {
public:
  struct Statistics {
    ThreadAwareSVFGBuilder::Statistics overlay;
    SparseFlowSensitivePTA::Statistics solver;
  };

  WholeProgramSparseRefinement() = default;
  ~WholeProgramSparseRefinement();

  WholeProgramSparseRefinement(const WholeProgramSparseRefinement &) = delete;
  WholeProgramSparseRefinement &
  operator=(const WholeProgramSparseRefinement &) = delete;

  const Statistics &build(llvm::Module &module, const mhp::IMHPAnalysis &mhp,
                          const mhp::LockSetAnalysis *locks = nullptr,
                          bool usePointerAnalysis = true);

  const SparseFlowSensitivePTA *solver() const { return solver_.get(); }
  const SVFG *graph() const { return svfg_.get(); }
  const Statistics &statistics() const { return stats_; }

private:
  // Declaration order preserves the source graphs until their non-owning
  // overlay and solver have been destroyed.
  std::unique_ptr<ICFG> icfg_;
  std::unique_ptr<SVFG> svfg_;
  std::unique_ptr<ThreadAwareSVFGBuilder> overlay_;
  std::unique_ptr<SparseFlowSensitivePTA> solver_;
  Statistics stats_;
};

} // namespace lotus::analysis

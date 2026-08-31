#include "Concurrency/ValueFlow/WholeProgramSparseRefinement.h"

#include "Concurrency/Utils/ThreadAPI.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"

namespace lotus::analysis {

WholeProgramSparseRefinement::~WholeProgramSparseRefinement() = default;

const WholeProgramSparseRefinement::Statistics &
WholeProgramSparseRefinement::build(llvm::Module &module,
                                    const mhp::IMHPAnalysis &mhp,
                                    const mhp::LockSetAnalysis *locks) {
  return build(module, mhp, locks, Config{});
}

const WholeProgramSparseRefinement::Statistics &
WholeProgramSparseRefinement::build(llvm::Module &module,
                                    const mhp::IMHPAnalysis &mhp,
                                    const mhp::LockSetAnalysis *locks,
                                    Config refinementConfig) {
  solver_.reset();
  overlay_.reset();
  mainMHP_.reset();
  mainThreadTree_.reset();
  slice_.reset();
  slicer_.reset();
  preThreadTree_.reset();
  svfg_.reset();
  icfg_.reset();
  stats_ = {};

  icfg_ = std::make_unique<ICFG>();
  ICFGBuilder icfgBuilder(icfg_.get());
  icfgBuilder.build(&module);

  SVFGBuilderConfig config;
  config.buildMSSA = true;
  config.usePointerAnalysis = refinementConfig.usePointerAnalysis;
  config.memoryPartition = refinementConfig.memoryPartition;
  SVFGBuilder svfgBuilder(config);
  svfg_.reset(svfgBuilder.build(icfg_.get()));
  stats_.memoryRegions = svfgBuilder.getMemoryRegionPartitionStatistics();

  preThreadTree_ = std::make_unique<ThreadCreationTree>(
      module, *ThreadAPI::getThreadAPI(), refinementConfig.threadContextLimit);
  stats_.preThreads = preThreadTree_->statistics();
  overlay_ = std::make_unique<ThreadAwareSVFGBuilder>(
      *svfg_, mhp, locks, nullptr, preThreadTree_.get());
  stats_.preOverlay = overlay_->build();

  if (refinementConfig.mode == Mode::MultiStageSlicing) {
    slicer_ =
        std::make_unique<MultiStageSlicer>(*svfg_, preThreadTree_.get());
    slice_ = slicer_->slice();
    stats_.slicing = slicer_->statistics();

    mainThreadTree_ = std::make_unique<ThreadCreationTree>(
        module, *ThreadAPI::getThreadAPI(), refinementConfig.threadContextLimit,
        &slicer_->synchronizationInstructions());
    stats_.mainThreads = mainThreadTree_->statistics();
    mainMHP_ = std::make_unique<TCTMHPAnalysis>(
        mhp, *mainThreadTree_, &slicer_->synchronizationInstructions());

    // Remove the pre-analysis overlay and independently re-query MHP/lock
    // facts for the retained main-phase graph.
    overlay_.reset();
    overlay_ = std::make_unique<ThreadAwareSVFGBuilder>(
        *svfg_, *mainMHP_, locks, slice_.get(), mainThreadTree_.get());
    stats_.overlay = overlay_->build();
  } else {
    stats_.overlay = stats_.preOverlay;
    stats_.mainThreads = stats_.preThreads;
    stats_.slicing.originalNodes = svfg_->getNumNodes();
    stats_.slicing.candidateNodes = svfg_->getNumNodes();
    stats_.slicing.synchronizationNodes = svfg_->getNumNodes();
    stats_.slicing.pointsToNodes = svfg_->getNumNodes();
  }

  solver_ = std::make_unique<SparseFlowSensitivePTA>(*svfg_, slice_.get());
  stats_.solver = solver_->solve();
  return stats_;
}

} // namespace lotus::analysis

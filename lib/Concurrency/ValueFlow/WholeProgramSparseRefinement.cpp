#include "Concurrency/ValueFlow/WholeProgramSparseRefinement.h"

#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"

namespace lotus::analysis {

WholeProgramSparseRefinement::~WholeProgramSparseRefinement() = default;

const WholeProgramSparseRefinement::Statistics &
WholeProgramSparseRefinement::build(llvm::Module &module,
                                    const mhp::IMHPAnalysis &mhp,
                                    const mhp::LockSetAnalysis *locks,
                                    bool usePointerAnalysis) {
  solver_.reset();
  overlay_.reset();
  svfg_.reset();
  icfg_.reset();
  stats_ = {};

  icfg_ = std::make_unique<ICFG>();
  ICFGBuilder icfgBuilder(icfg_.get());
  icfgBuilder.build(&module);

  SVFGBuilderConfig config;
  config.buildMSSA = true;
  config.usePointerAnalysis = usePointerAnalysis;
  SVFGBuilder svfgBuilder(config);
  svfg_.reset(svfgBuilder.build(icfg_.get()));

  overlay_ = std::make_unique<ThreadAwareSVFGBuilder>(*svfg_, mhp, locks);
  stats_.overlay = overlay_->build();

  solver_ = std::make_unique<SparseFlowSensitivePTA>(*svfg_);
  stats_.solver = solver_->solve();
  return stats_;
}

} // namespace lotus::analysis

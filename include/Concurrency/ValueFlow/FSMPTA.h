/**
 * @file FSMPTA.h
 * @brief Concurrent composition of FlowSensitivePTA over thread-aware SVFG.
 */
#pragma once

#include "Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h"

namespace lotus::analysis {

class FSMPTA {
public:
  struct Config {
    const FilteredSVFGView *scope = nullptr;
    lotus::alias::PointsToSetBackend setBackend =
        lotus::alias::PointsToSetBackend::Mutable;
  };

  explicit FSMPTA(const SVFG &threadAwareGraph);
  FSMPTA(const SVFG &threadAwareGraph, Config config);

  const lotus::alias::FlowSensitivePTA::Statistics &solve() {
    return solver_.solve();
  }
  const lotus::alias::FlowSensitivePTA &core() const { return solver_; }
  lotus::alias::FlowSensitivePTA &core() { return solver_; }

private:
  lotus::alias::FlowSensitivePTA solver_;
};

} // namespace lotus::analysis

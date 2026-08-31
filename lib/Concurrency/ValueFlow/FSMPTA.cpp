#include "Concurrency/ValueFlow/FSMPTA.h"

namespace lotus::analysis {

FSMPTA::FSMPTA(const SVFG &threadAwareGraph)
    : FSMPTA(threadAwareGraph, Config{}) {}

FSMPTA::FSMPTA(const SVFG &threadAwareGraph, Config config)
    : solver_(threadAwareGraph, lotus::alias::FlowSensitivePTA::Config{
                                    config.setBackend, config.scope,
                                    std::move(config.connectIndirectCall)}) {}

} // namespace lotus::analysis

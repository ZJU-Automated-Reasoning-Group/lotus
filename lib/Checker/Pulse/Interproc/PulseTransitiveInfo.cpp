#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"

namespace pulse {

TransitiveInfo TransitiveInfo::merge(const TransitiveInfo &t1,
                                     const TransitiveInfo &t2) {
  TransitiveInfo merged;

  // Merge transitive accesses
  merged.transitive_accesses_ = t1.transitive_accesses_;
  merged.transitive_accesses_.insert(merged.transitive_accesses_.end(),
                                     t2.transitive_accesses_.begin(),
                                     t2.transitive_accesses_.end());

  // Merge call resolutions
  merged.call_resolutions_ = t1.call_resolutions_;
  merged.call_resolutions_.insert(merged.call_resolutions_.end(),
                                  t2.call_resolutions_.begin(),
                                  t2.call_resolutions_.end());

  return merged;
}

} // namespace pulse

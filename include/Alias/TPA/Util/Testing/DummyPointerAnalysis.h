#pragma once

#include "Alias/TPA/PointerAnalysis/Analysis/PointerAnalysis.h"

namespace util {

// A dummy pointer analysis which is mostly useful for unit test
class DummyPointerAnalysis : public PointerAnalysis {
public:
  DummyPointerAnalysis(PointerManager &pm, MemoryManager &mm,
                       const ExternalPointerTable &t)
      : PointerAnalysis(pm, mm, t) {}

  void injectEnv(const Pointer *ptr, const MemoryLocation *loc) {
    env.insert(ptr, loc);
  }
};

} // namespace util

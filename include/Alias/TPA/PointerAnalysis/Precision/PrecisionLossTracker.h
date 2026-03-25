#pragma once

#include "Alias/TPA/PointerAnalysis/Support/ProgramPointSet.h"

#include <vector>

namespace tpa {

class GlobalState;
class Pointer;

class PrecisionLossTracker {
private:
  using PointerList = std::vector<const Pointer *>;
  using ProgramPointList = std::vector<ProgramPoint>;

  const GlobalState &globalState;

  ProgramPointList getProgramPointsFromPointers(const PointerList &);

public:
  PrecisionLossTracker(const GlobalState &g) : globalState(g) {}

  ProgramPointSet trackImprecision(const PointerList &);
};

} // namespace tpa

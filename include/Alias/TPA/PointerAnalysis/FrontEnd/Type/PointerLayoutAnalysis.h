#pragma once

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/PointerLayoutMap.h"

namespace tpa {

class CastMap;
class TypeSet;

class PointerLayoutAnalysis {
private:
  const CastMap &castMap;

public:
  PointerLayoutAnalysis(const CastMap &c) : castMap(c) {}

  PointerLayoutMap runOnTypes(const TypeSet &);
};

} // namespace tpa

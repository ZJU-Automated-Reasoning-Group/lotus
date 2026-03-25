#pragma once

#include "Alias/TPA/PointerAnalysis/Support/ProgramPoint.h"
#include "Alias/TPA/PointerAnalysis/Support/Store.h"

namespace tpa {

class EvalSuccessor {
private:
  ProgramPoint pp;
  const Store *store;

  EvalSuccessor(const ProgramPoint &p, const Store *s) : pp(p), store(s) {}

public:
  bool isTopLevel() const { return store == nullptr; }
  const ProgramPoint &getProgramPoint() const { return pp; }
  const Store *getStore() const { return store; }

  friend class EvalResult;
};

} // namespace tpa

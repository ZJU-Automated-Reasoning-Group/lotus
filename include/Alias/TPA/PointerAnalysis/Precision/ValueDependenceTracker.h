#pragma once

#include "Alias/TPA/PointerAnalysis/Support/CallGraph.h"
#include "Alias/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Alias/TPA/PointerAnalysis/Support/ProgramPointSet.h"

#include <vector>

namespace tpa {

class SemiSparseProgram;

class ValueDependenceTracker {
private:
  using CallGraphType = CallGraph<ProgramPoint, FunctionContext>;
  const CallGraphType &callGraph;

  const SemiSparseProgram &ssProg;

public:
  ValueDependenceTracker(const CallGraphType &c, const SemiSparseProgram &s)
      : callGraph(c), ssProg(s) {}

  ProgramPointSet getValueDependencies(const ProgramPoint &) const;
};

} // namespace tpa

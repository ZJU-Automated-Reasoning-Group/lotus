#pragma once

#include "Alias/TPA/PointerAnalysis/Support/CallGraph.h"
#include "Alias/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/TPA/PointerAnalysis/Support/FunctionContext.h"
#include "Alias/TPA/PointerAnalysis/Support/ProgramPointSet.h"

namespace annotation {
class ExternalPointerTable;
} // namespace annotation

namespace tpa {

class MemoryManager;
class PointerManager;
class SemiSparseProgram;

class TrackerGlobalState {
private:
  const PointerManager &ptrManager;
  const MemoryManager &memManager;

  const SemiSparseProgram &ssProg;
  const Env &env;
  const CallGraph<ProgramPoint, FunctionContext> &callGraph;
  const annotation::ExternalPointerTable &extTable;

  ProgramPointSet &imprecisionSources;
  ProgramPointSet visited;

public:
  TrackerGlobalState(const PointerManager &pm, const MemoryManager &mm,
                     const SemiSparseProgram &s, const Env &e,
                     const CallGraph<ProgramPoint, FunctionContext> &c,
                     const annotation::ExternalPointerTable &x,
                     ProgramPointSet &i)
      : ptrManager(pm), memManager(mm), ssProg(s), env(e), callGraph(c),
        extTable(x), imprecisionSources(i) {}

  const PointerManager &getPointerManager() const { return ptrManager; }
  const MemoryManager &getMemoryManager() const { return memManager; }

  const SemiSparseProgram &getSemiSparseProgram() const { return ssProg; }
  const Env &getEnv() const { return env; }
  const CallGraph<ProgramPoint, FunctionContext> &getCallGraph() const {
    return callGraph;
  }
  const annotation::ExternalPointerTable &getExternalPointerTable() const {
    return extTable;
  }

  bool insertVisitedLocation(const ProgramPoint &pp) {
    return visited.insert(pp);
  }

  void addImprecisionSource(const ProgramPoint &pp) {
    imprecisionSources.insert(pp);
  }
};

} // namespace tpa

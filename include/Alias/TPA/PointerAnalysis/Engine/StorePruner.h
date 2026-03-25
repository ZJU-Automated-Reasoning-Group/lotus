#pragma once

#include "Alias/TPA/PointerAnalysis/Support/Env.h"
#include "Alias/TPA/PointerAnalysis/Support/ProgramPoint.h"
#include "Alias/TPA/PointerAnalysis/Support/Store.h"
#include "Alias/TPA/Util/DataStructure/VectorSet.h"

#include <unordered_map>
#include <unordered_set>

namespace tpa {

class MemoryManager;
class PointerManager;

class StorePruner {
private:
  const Env &env;
  const PointerManager &ptrManager;
  const MemoryManager &memManager;

  using ObjectSet = std::unordered_set<const MemoryObject *>;
  ObjectSet getRootSet(const Store &, const ProgramPoint &);
  void findAllReachableObjects(const Store &, ObjectSet &);
  Store filterStore(const Store &, const ObjectSet &);

public:
  StorePruner(const Env &e, const PointerManager &p, const MemoryManager &m)
      : env(e), ptrManager(p), memManager(m) {}

  Store pruneStore(const Store &, const ProgramPoint &);
};

} // namespace tpa

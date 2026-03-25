//===-- Verification/Sifa/Caches/ProcedureResourceCache.h
//------------------===//
//
// Cache for ProcedureResources per procedure (ported from Ultimate Sifa).
//
// Ultimate's ProcedureResourceCache is keyed by procedure name and uses
// CallGraph for LOIs/enter-calls. In lotus we key by llvm::Function* and
// accept LOIs (and optionally enter-calls) per request; results are cached
// so repeated resourcesOf() for the same function and LOIs reuse the DAG.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CACHES_PROCEDURERESOURCECACHE_H
#define LOTUS_VERIFICATION_SIFA_CACHES_PROCEDURERESOURCECACHE_H

#include "llvm/IR/Function.h"

#include "Verification/Sifa/CallGraph.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

#include <unordered_map>
#include <vector>

namespace llvm {
class BasicBlock;
class Module;
} // namespace llvm

namespace lotus {
namespace sifa {

/// Cache for ProcedureResources keyed by function (and optionally CallGraph).
/// Ultimate-aligned: when constructed with CallGraph, resourcesOf(F) uses
/// CallGraph.locationsOfInterest(F) and CallGraph.successorsOfInterest(F).
class ProcedureResourceCache {
public:
  explicit ProcedureResourceCache(SifaStats &stats)
      : stats_(stats), callGraph_(nullptr), M_(nullptr) {}

  /// Ultimate-aligned: use CallGraph for LOIs and enterCallsOfInterest per
  /// procedure.
  ProcedureResourceCache(SifaStats &stats, const CallGraph &cg,
                         const llvm::Module &M)
      : stats_(stats), callGraph_(&cg), M_(&M) {}

  /// Returns (possibly cached) ProcedureResources for \p F.
  /// When constructed with CallGraph, uses cg.locationsOfInterest(F) and
  /// cg.successorsOfInterest(F). Otherwise use the overload with explicit LOIs.
  const ProcedureResources &resourcesOf(const llvm::Function &F);

  /// Ultimate-aligned: resourcesOf(procedureName). Requires M and CallGraph.
  /// Looks up function by name and returns resourcesOf(*F).
  const ProcedureResources &resourcesOf(const std::string &procedureName);

  /// Overload: explicit \p locationsOfInterest (e.g. when not using CallGraph).
  const ProcedureResources &
  resourcesOf(const llvm::Function &F,
              const std::vector<llvm::BasicBlock *> &locationsOfInterest);

private:
  struct Key {
    const llvm::Function *F = nullptr;
    std::vector<llvm::BasicBlock *> LOIs;
    std::vector<const llvm::Function *> enterCalls;

    bool operator==(const Key &o) const;
  };

  struct KeyHash {
    std::size_t operator()(const Key &k) const;
  };

  SifaStats &stats_;
  const CallGraph *callGraph_ = nullptr;
  const llvm::Module *M_ = nullptr;
  std::unordered_map<Key, ProcedureResources, KeyHash> cache_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_CACHES_PROCEDURERESOURCECACHE_H

#include "Verification/Sifa/Caches/ProcedureResourceCache.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <cstddef>
#include <stdexcept>

using namespace lotus::sifa;

bool ProcedureResourceCache::Key::operator==(const Key &o) const {
  if (F != o.F || LOIs.size() != o.LOIs.size() ||
      enterCalls.size() != o.enterCalls.size()) {
    return false;
  }
  for (size_t i = 0; i < LOIs.size(); ++i)
    if (LOIs[i] != o.LOIs[i])
      return false;
  for (size_t i = 0; i < enterCalls.size(); ++i)
    if (enterCalls[i] != o.enterCalls[i])
      return false;
  return true;
}

std::size_t ProcedureResourceCache::KeyHash::operator()(const Key &k) const {
  std::size_t h = std::hash<const llvm::Function *>()(k.F);
  for (auto *bb : k.LOIs)
    h ^= (std::hash<llvm::BasicBlock *>()(bb) + 0x9e3779b9 + (h << 6) +
          (h >> 2));
  for (auto *callee : k.enterCalls)
    h ^= (std::hash<const llvm::Function *>()(callee) + 0x9e3779b9 + (h << 6) +
          (h >> 2));
  return h;
}

const ProcedureResources &
ProcedureResourceCache::resourcesOf(const std::string &procedureName) {
  if (!M_ || !callGraph_) {
    throw std::logic_error("ProcedureResourceCache::resourcesOf(string) "
                           "requires Module and CallGraph");
  }
  llvm::Function *F = M_->getFunction(procedureName);
  if (!F) {
    throw std::invalid_argument("Procedure not found: " + procedureName);
  }
  return resourcesOf(*F);
}

const ProcedureResources &
ProcedureResourceCache::resourcesOf(const llvm::Function &F) {
  if (callGraph_) {
    std::vector<llvm::BasicBlock *> lois;
    for (const llvm::BasicBlock *bb : callGraph_->locationsOfInterest(F))
      lois.push_back(const_cast<llvm::BasicBlock *>(bb));
    std::vector<const llvm::Function *> enterCalls;
    for (const llvm::Function *callee : callGraph_->successorsOfInterest(F))
      enterCalls.push_back(callee);
    Key k;
    k.F = &F;
    k.LOIs = lois;
    k.enterCalls = enterCalls;
    auto it = cache_.find(k);
    if (it != cache_.end())
      return it->second;
    auto inserted = cache_.emplace(
        std::move(k), ProcedureResources(stats_, F, lois, enterCalls));
    return inserted.first->second;
  }
  Key k;
  k.F = &F;
  auto it = cache_.find(k);
  if (it != cache_.end())
    return it->second;
  auto inserted =
      cache_.emplace(std::move(k), ProcedureResources(stats_, F, {}));
  return inserted.first->second;
}

const ProcedureResources &ProcedureResourceCache::resourcesOf(
    const llvm::Function &F,
    const std::vector<llvm::BasicBlock *> &locationsOfInterest) {
  Key k;
  k.F = &F;
  k.LOIs = locationsOfInterest;

  auto it = cache_.find(k);
  if (it != cache_.end()) {
    return it->second;
  }

  auto inserted = cache_.emplace(
      std::move(k), ProcedureResources(stats_, F, locationsOfInterest));
  return inserted.first->second;
}

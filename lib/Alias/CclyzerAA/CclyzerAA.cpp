/// Optional cclyzer++ backend implementation. Built only when
/// LOTUS_USE_CCLYZER=ON.

#include "Alias/CclyzerAA/CclyzerAA.h"

#include "PointerAnalysis.h"

#include <set>

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>

namespace lotus {
namespace cclyzer {

struct CclyzerAA::Impl {
  std::unique_ptr<::cclyzer::LegacyPointerAnalysis> pass;
};

CclyzerAA::CclyzerAA() : _impl(std::make_unique<Impl>()) {}

CclyzerAA::~CclyzerAA() = default;

bool CclyzerAA::run(llvm::Module &M) {
  _initialized = false;
  _impl->pass = std::make_unique<::cclyzer::LegacyPointerAnalysis>();
  llvm::legacy::PassManager PM;
  PM.add(_impl->pass.get());
  PM.run(M);
  _initialized = true;
  return true;
}

llvm::AliasResult CclyzerAA::alias(const llvm::Value *v1,
                                   const llvm::Value *v2) {
  if (!_initialized || !_impl->pass)
    return llvm::AliasResult::MayAlias;
  auto loc1 = llvm::MemoryLocation(
      v1, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
  auto loc2 = llvm::MemoryLocation(
      v2, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
  return alias(loc1, loc2);
}

llvm::AliasResult CclyzerAA::alias(const llvm::MemoryLocation &loc1,
                                   const llvm::MemoryLocation &loc2) {
  if (!_initialized || !_impl->pass)
    return llvm::AliasResult::MayAlias;
  llvm::AAQueryInfo AAQI;
  return _impl->pass->getResult().alias(loc1, loc2, AAQI);
}

bool CclyzerAA::getPointsToSet(const llvm::Value *ptr,
                               std::vector<const llvm::Value *> &ptsSet) {
  ptsSet.clear();
  if (!_initialized || !_impl->pass)
    return false;
  const auto &result = _impl->pass->getResult();
  const auto &varPts = result.getVariablePointsTo();
  const auto &allocSites = result.getAllocationSites();
  std::set<boost::flyweight<std::string>> aliasSets;
  for (const auto &t : varPts) {
    if (std::get<3>(t) == ptr)
      aliasSets.insert(std::get<1>(t));
  }
  for (const auto &t : allocSites) {
    if (aliasSets.count(std::get<3>(t)))
      ptsSet.push_back(std::get<1>(t));
  }
  return true;
}

} // namespace cclyzer
} // namespace lotus

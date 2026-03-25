#include "Alias/DFPA/Result.h"

using namespace dfpa;

const DFPATargetSet *DFPAResult::getTargets(const llvm::CallBase *CB) const {
  auto It = results_.find(CB);
  if (It == results_.end())
    return nullptr;
  return &It->second.targets;
}

bool DFPAResult::isPrecise(const llvm::CallBase *CB) const {
  auto It = results_.find(CB);
  if (It == results_.end())
    return false;
  return It->second.precise;
}

void DFPAResult::clear() {
  results_.clear();
  stats_ = DFPAStats();
}

void DFPAResult::setTargets(const llvm::CallBase *CB,
                            const DFPATargetInfo &Info) {
  results_[CB] = Info;
}

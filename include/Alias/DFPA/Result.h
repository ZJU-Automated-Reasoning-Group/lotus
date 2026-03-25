#pragma once

#include <map>
#include <set>

namespace llvm {
class CallBase;
class Function;
} // namespace llvm

namespace dfpa {

using DFPATargetSet = std::set<llvm::Function *>;

struct DFPATargetInfo {
  DFPATargetSet targets;
  bool precise = false;
  bool refined = false;
  bool had_unknown_flow = false;
};

struct DFPAStats {
  std::size_t num_indirect_calls = 0;
  std::size_t num_refined_calls = 0;
  std::size_t num_precise_calls = 0;
  std::size_t num_budget_fallbacks = 0;
  std::size_t num_unknown_slot_degradations = 0;
  std::size_t coarse_total_targets = 0;
  std::size_t refined_total_targets = 0;

  double coarseAvgTargets() const {
    if (num_indirect_calls == 0)
      return 0.0;
    return static_cast<double>(coarse_total_targets) /
           static_cast<double>(num_indirect_calls);
  }

  double refinedAvgTargets() const {
    if (num_indirect_calls == 0)
      return 0.0;
    return static_cast<double>(refined_total_targets) /
           static_cast<double>(num_indirect_calls);
  }
};

class DFPAResult {
public:
  using ResultMap = std::map<const llvm::CallBase *, DFPATargetInfo>;

  const DFPATargetSet *getTargets(const llvm::CallBase *CB) const;
  bool isPrecise(const llvm::CallBase *CB) const;
  const ResultMap &getAllTargets() const { return results_; }
  const DFPAStats &getStats() const { return stats_; }

  void clear();
  void setTargets(const llvm::CallBase *CB, const DFPATargetInfo &Info);
  void setStats(const DFPAStats &Stats) { stats_ = Stats; }

private:
  ResultMap results_;
  DFPAStats stats_;
};

} // namespace dfpa

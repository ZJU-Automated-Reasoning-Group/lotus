#pragma once

#include "Alias/TPA/PointerAnalysis/Support/ProgramPoint.h"

#include <unordered_set>

namespace tpa {

class ProgramPointSet {
private:
  using SetType = std::unordered_set<ProgramPoint>;
  SetType ppSet;

public:
  using const_iterator = SetType::const_iterator;

  ProgramPointSet() = default;

  ProgramPointSet(const ProgramPointSet &) = delete;
  ProgramPointSet(ProgramPointSet &&) noexcept = default;
  ProgramPointSet &operator=(const ProgramPointSet &) = delete;
  ProgramPointSet &operator=(ProgramPointSet &&) = delete;

  bool insert(const ProgramPoint &pp) { return ppSet.insert(pp).second; }

  void swap(ProgramPointSet &rhs) { ppSet.swap(rhs.ppSet); }

  const_iterator begin() const { return ppSet.begin(); }
  const_iterator end() const { return ppSet.end(); }
  size_t size() const { return ppSet.size(); }
  bool empty() const { return ppSet.empty(); }
};

} // namespace tpa
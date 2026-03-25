#pragma once

#include "Alias/TPA/PointerAnalysis/Engine/EvalSuccessor.h"
#include "Alias/TPA/PointerAnalysis/Support/Store.h"

#include <memory>
#include <vector>

namespace tpa {

class EvalResult {
private:
  std::vector<std::unique_ptr<Store>> storeVec;

  using SuccessorList = std::vector<EvalSuccessor>;
  SuccessorList succs;

public:
  using const_iterator = typename SuccessorList::const_iterator;

  EvalResult() = default;

  EvalResult(const EvalResult &) = delete;
  EvalResult(EvalResult &&) noexcept = default;
  EvalResult &operator=(const EvalResult &) = delete;
  EvalResult &operator=(EvalResult &&) = delete;

  template <typename... Args> Store &getNewStore(Args &&...a) {
    storeVec.emplace_back(std::make_unique<Store>(std::forward<Args>(a)...));
    return *storeVec.back();
  }

  void addTopLevelProgramPoint(const ProgramPoint &pp) {
    succs.push_back(EvalSuccessor(pp, nullptr));
  }

  void addMemLevelProgramPoint(const ProgramPoint &pp, const Store &store) {
    succs.push_back(EvalSuccessor(pp, &store));
  }

  const_iterator begin() const { return succs.begin(); }
  const_iterator end() const { return succs.end(); }
  bool empty() const { return succs.empty(); }
  size_t size() const { return succs.size(); }
};

} // namespace tpa

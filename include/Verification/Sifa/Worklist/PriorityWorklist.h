//===-- Verification/Sifa/Worklist/PriorityWorklist.h ---------------------===//
//
// Priority worklist based on a custom order (ported from Ultimate Sifa).
//
// Only work items present in the custom order can be inserted.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_WORKLIST_PRIORITYWORKLIST_H
#define LOTUS_VERIFICATION_SIFA_WORKLIST_PRIORITYWORKLIST_H

#include "Verification/Sifa/Worklist/IWorklistWithInputs.h"

#include <functional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace sifa {

template <typename W, typename I>
class PriorityWorklist final : public IWorklistWithInputs<W, I> {
public:
  using MergeFn = std::function<I(const I &, const I &)>;

  PriorityWorklist(std::vector<W> order, MergeFn merge)
      : idxToWork_(std::move(order)), merge_(std::move(merge)) {
    for (std::size_t i = 0; i < idxToWork_.size(); ++i) {
      workToIdx_.emplace(idxToWork_[i], static_cast<int>(i));
    }
  }

  void add(W work, I newInput) override {
    const auto it = workToIdx_.find(work);
    if (it == workToIdx_.end()) {
      throw std::invalid_argument(
          "Tried to insert element unknown in custom order");
    }
    const int idx = it->second;

    auto inIt = inputs_.find(idx);
    if (inIt != inputs_.end()) {
      inIt->second = merge_(inIt->second, newInput);
      return;
    }
    pq_.push(idx);
    inputs_.emplace(idx, std::move(newInput));
  }

  bool advance() override {
    if (pq_.empty()) {
      hasCurrent_ = false;
      return false;
    }
    const int idx = pq_.top();
    pq_.pop();
    currentWork_ = idxToWork_.at(static_cast<std::size_t>(idx));
    auto it = inputs_.find(idx);
    currentInput_ = it->second;
    inputs_.erase(it);
    hasCurrent_ = true;
    return true;
  }

  W getWork() const override {
    ensureAdvanced();
    return currentWork_;
  }

  I getInput() const override {
    ensureAdvanced();
    return currentInput_;
  }

  /// Ultimate-aligned: toString() — string representation (e.g. for logging).
  std::string toString() const {
    return "PriorityWorklist(size=" +
           std::to_string(pq_.size() + (hasCurrent_ ? 1u : 0u)) + ")";
  }

private:
  void ensureAdvanced() const {
    if (!hasCurrent_) {
      throw std::logic_error("Never called advance() on this worklist.");
    }
  }

  std::vector<W> idxToWork_;
  std::unordered_map<W, int> workToIdx_;

  std::priority_queue<int, std::vector<int>, std::greater<int>> pq_;
  std::unordered_map<int, I> inputs_;

  MergeFn merge_;

  bool hasCurrent_ = false;
  W currentWork_{};
  I currentInput_{};
};

} // namespace sifa
} // namespace lotus

#include "llvm/IR/Function.h"

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/RegexDag/RegexDagNode.h"
#include "Verification/Sifa/SifaSymAbs.h"
extern template class lotus::sifa::PriorityWorklist<
    lotus::sifa::RegexDagNode<lotus::sifa::Transition> *, bool>;
extern template class lotus::sifa::PriorityWorklist<
    lotus::sifa::RegexDagNode<lotus::sifa::Transition> *,
    lotus::sifa::SymAbsState>;
extern template class lotus::sifa::PriorityWorklist<const llvm::Function *,
                                                    bool>;

#endif // LOTUS_VERIFICATION_SIFA_WORKLIST_PRIORITYWORKLIST_H

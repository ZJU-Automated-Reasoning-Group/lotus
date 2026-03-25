// Use-after-free detector: typestate FSM init -> free -> BUG (use after free).
// defineStates() registers states and transitions (free on kfree; use triggers BUG).
#include "Checker/FiTx/Detector/UAF_Detector.h"

#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class UseAfterFreeDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    UseAfterFree::defineStates(manager);  // init, free, BUG; free_func, use, store_any.
    addStateManager(manager);
  }
};

}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new UseAfterFreeDetector()};

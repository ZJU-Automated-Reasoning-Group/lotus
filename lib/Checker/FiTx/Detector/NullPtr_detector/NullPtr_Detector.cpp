#include "Checker/FiTx/Detector/NullPtr_Detector.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace NullPointer {
void defineStates(fitx::StateManager& manager) {
  auto init_args = fitx::StateArgs("init", fitx::StateType::INIT);
  fitx::State& init = manager.createState(init_args);

  auto null_args = fitx::StateArgs("null");
  fitx::State& null = manager.createState(null_args);

  auto use_args = fitx::StateArgs(
      "used", fitx::StateType::BUG,
      fitx::BugNotificationTiming::IMMEDIATE, false);
  fitx::State& use = manager.createState(use_args);

  auto use_rule = fitx::UseValueTransitionRule();
  manager.addTransition(null, use, use_rule);

  auto store_non_null_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::NON_NULL_VAL);
  manager.addTransition(null, init, store_non_null_rule);

  auto store_null_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::NULL_VAL);
  manager.addTransition(init, null, store_null_rule);
}
}  // namespace NullPointer

namespace {
class NullPtrDereferenceDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    NullPointer::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

// passes defined in All_Detector.cpp when building with All_Detector

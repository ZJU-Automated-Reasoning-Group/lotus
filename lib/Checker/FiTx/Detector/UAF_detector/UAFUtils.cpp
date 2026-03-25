// UAF typestate definition: init, free, BUG (use-after-free). Transitions:
// init -> free on call to free_funcs; free -> BUG on use (load); free -> init on store ANY.
#include "Checker/FiTx/Detector/UAF_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

namespace UseAfterFree {
/// Suppresses propagation on calls whose name contains "put" (refcount helpers).
class OneshotCallConstraint : public fitx::StatefulConstraint {
 public:
virtual ~OneshotCallConstraint() = default;
  bool shouldPropagateOnCallInst(
      std::shared_ptr<fitx::CallInst> inst) override {
    std::shared_ptr<fitx::Function> called_func = inst->CalledFunction();
    std::shared_ptr<fitx::Function> parent =
        inst->Parent().lock()->Parent().lock();
    if (!called_func || !parent) return true;

    // If "put"  is in the function name, it is most-likely reference counted
    // function. Hence, do not consider them
    if (called_func->Name().find("put") != std::string::npos) {
      return false;
    }
    return true;
  }
};

void defineStates(fitx::StateManager& manager) {
  fitx::State& init = manager.getInitState();

  fitx::StateArgs free_args("free");
  fitx::State& free = manager.createState(free_args);

  // init -> free when pointer is passed to free_funcs (e.g. kfree).
  auto free_func_rule = fitx::FunctionArgTransitionRule(free_funcs);
  manager.addTransition(init, free, free_func_rule);

  // free -> BUG (use-after-free) on load (UseValueTransitionRule).
  auto ubi_args =
      fitx::StateArgs("Used", fitx::StateType::BUG,
                           fitx::BugNotificationTiming::IMMEDIATE, false);
  fitx::State& uaf = manager.createState(ubi_args);

  auto use_rule = fitx::UseValueTransitionRule();
  manager.addTransition(free, uaf, use_rule);

  auto store_any_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::ANY);

  manager.addTransition(free, init, store_any_rule);
  manager.enableStatefulConstraint(std::make_shared<OneshotCallConstraint>());
}
}  // namespace UseAfterFree

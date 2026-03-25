#include "Checker/FiTx/Detector/DUL_Detector.h"
#include "Checker/FiTx/Frontend/PropagationConstraint.h"

class LockInUnlockConstraint : public fitx::StatefulConstraint {
 public:
  bool shouldPropagateOnCallInst(
      std::shared_ptr<fitx::CallInst> inst) override {
    std::shared_ptr<fitx::Function> called_func = inst->CalledFunction();
    std::shared_ptr<fitx::Function> parent =
        inst->Parent().lock()->Parent().lock();
    if (!called_func || !parent) return true;

    if (called_func->Name().find("_unlock") == std::string::npos) return true;

    if (parent->Name().find("_lock") != std::string::npos) return false;
    return true;
  }
};

namespace DoubleUnlock {
void defineStates(fitx::StateManager &manager) {
  // Create States
  fitx::State &init = manager.getInitState();

  auto lock_args = fitx::StateArgs("locked");
  fitx::State &lock = manager.createState(lock_args);

  auto unlock_args = fitx::StateArgs("unlocked");
  fitx::State &unlock = manager.createState(unlock_args);

  auto dl_args =
      fitx::StateArgs("double unlocked", fitx::StateType::BUG);
  fitx::State &double_unlock = manager.createState(dl_args);

  auto lock_rule = fitx::FunctionArgTransitionRule(lock_funcs_w_try);
  auto store_any_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::ANY);

  manager.addTransition(init, lock, lock_rule);

  auto unlock_rule = fitx::FunctionArgTransitionRule(unlock_funcs);
  manager.addTransition(lock, unlock, unlock_rule);
  manager.addTransition(unlock, double_unlock, unlock_rule);

  manager.addTransition(unlock, lock, lock_rule);
  manager.addTransition(unlock, init, store_any_rule);

  manager.enableStatefulConstraint(std::make_shared<LockInUnlockConstraint>());
}
}  // namespace DoubleUnlock

#include "Checker/FiTx/Detector/DL_Detector.h"
#include "Checker/FiTx/Detector/Lock.h"
#include "Checker/FiTx/Frontend/State.h"

class TryLockConstraint : public fitx::StatefulConstraint {
 public:
virtual ~TryLockConstraint() = default;
  bool shouldPropagateOnCallInst(
      std::shared_ptr<fitx::CallInst> inst) override {
    std::shared_ptr<fitx::Function> called_func = inst->CalledFunction();
    std::shared_ptr<fitx::Function> parent =
        inst->Parent().lock()->Parent().lock();
    if (!called_func || !parent) return true;

    return called_func->Name().find("_trylock") == std::string::npos;
  }
};

namespace DoubleLock {
void define_states(fitx::StateManager& manager) {
  // Create States
  fitx::State& init = manager.getInitState();

  auto lock_args = fitx::StateArgs("locked");
  fitx::State& lock = manager.createState(lock_args);

  auto dl_args =
      fitx::StateArgs("double locked", fitx::StateType::BUG);
  fitx::State& double_lock = manager.createState(dl_args);

  auto lock_rule = fitx::FunctionArgTransitionRule(lock_funcs_w_try);
  auto lock_rule_wo_try = fitx::FunctionArgTransitionRule(lock_funcs);
  manager.addTransition(init, lock, lock_rule_wo_try);
  manager.addTransition(lock, double_lock, lock_rule_wo_try);

  auto unlock_rule = fitx::FunctionArgTransitionRule(unlock_funcs);
  auto store_any_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::ANY);

  manager.addTransition(lock, init, unlock_rule);
  manager.addTransition(lock, init, store_any_rule);

  manager.enableStatefulConstraint(std::make_shared<TryLockConstraint>());
}
}  // namespace DoubleLock

#include "Checker/FiTx/Detector/Leak_Detector.h"
#include "Checker/FiTx/Detector/Alloc.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/FiTx/Frontend/StateTransition.h"

namespace MemoryLeak {
void defineStates(fitx::StateManager& manager) {
  // Create States
  fitx::State& init = manager.getInitState();

  auto alloc_args =
      fitx::StateArgs("allocated", fitx::StateType::BUG,
                           fitx::BugNotificationTiming::MODULE_END, false,
                           fitx::NON_RETURN);

  fitx::State& allocated = manager.createState(alloc_args);

  auto store_alloc_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::CALL_FUNC, alloc_funcs);

  manager.addTransition(init, allocated, store_alloc_rule);

  auto store_any_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::ANY);
  manager.addTransition(allocated, init, store_any_rule);

  auto alias_rule = fitx::AliasValueTransitionRule();
  manager.addTransition(allocated, init, alias_rule);

  auto call_free_rule = fitx::FunctionArgTransitionRule(free_funcs);
  manager.addTransition(allocated, init, call_free_rule);

  auto store_related_rule = fitx::FunctionArgTransitionRule(store_related);
  manager.addTransition(allocated, init, store_related_rule);
}
}  // namespace MemoryLeak

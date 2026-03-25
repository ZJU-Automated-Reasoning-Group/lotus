#include "Checker/FiTx/Detector/Ref_Detector.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/FiTx/Frontend/StateTransition.h"

namespace ReferenceCounter {
void defineStates(fitx::StateManager& manager) {
  // Create States
  fitx::State& init = manager.getInitState();

  /* auto init_rule = fitx::FunctionArgTransitionRule(init_funcs); */
  auto inc_rule = fitx::FunctionArgTransitionRule(inc_funcs);
  auto dec_rule = fitx::FunctionArgTransitionRule(dec_funcs);

  fitx::State* prev = &init;
  for (int i = 0; i < 10; i++) {
    fitx::StateType type =
        i == 1 ? fitx::StateType::BUG : fitx::StateType::NORMAL;
    auto counted_args = fitx::StateArgs("counted " + std::to_string(i),
                                             type, fitx::MODULE_END);

    fitx::State& counted = manager.createState(counted_args);
    manager.addTransition(*prev, counted, inc_rule);
    manager.addTransition(counted, *prev, dec_rule);
    prev = &counted;
  }
}
}  // namespace ReferenceCounter

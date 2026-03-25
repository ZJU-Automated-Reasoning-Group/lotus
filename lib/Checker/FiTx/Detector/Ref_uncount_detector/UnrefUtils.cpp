#include "Checker/FiTx/Detector/Unref_Detector.h"

namespace UnreferenceCounter {
  void defineStates(fitx::StateManager& manager) {
    // Create States
    fitx::State& init = manager.getInitState();

    /* auto init_rule = fitx::FunctionArgTransitionRule(init_funcs); */
    auto inc_rule = fitx::FunctionArgTransitionRule(inc_funcs);
    auto dec_rule = fitx::FunctionArgTransitionRule(dec_funcs);

    fitx::State *prev = &init;
    for (int i = 0; i < 10; i++) {
      fitx::StateType type =
          i != 0 ? fitx::StateType::BUG : fitx::StateType::NORMAL;
      auto counted_args = fitx::StateArgs("uncounted " + std::to_string(i),
                                               type, fitx::MODULE_END);

      fitx::State &counted = manager.createState(counted_args);
      manager.addTransition(*prev, counted, dec_rule);
      manager.addTransition(counted, *prev, inc_rule);
      prev = &counted;
    }
  }

} // namespace UnreferenceCounter

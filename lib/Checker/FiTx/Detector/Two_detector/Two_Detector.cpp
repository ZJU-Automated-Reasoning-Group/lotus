#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Detector/DF_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class DoubleFreeDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    // Create States
    auto init_args = fitx::StateArgs("init", fitx::StateType::INIT);
    fitx::State &init = manager.createState(init_args);

    auto free_args = fitx::StateArgs("free");
    fitx::State &free = manager.createState(free_args);

    auto df_args =
        fitx::StateArgs("double free", fitx::StateType::BUG);
    fitx::State &double_free = manager.createState(df_args);

    // Create rule for Function Arg Transition (i.e. when the value is passed
    // as the argument of specified functions)
    auto free_func_rule = fitx::FunctionArgTransitionRule(free_funcs);

    manager.addTransition(init, free, free_func_rule);
    manager.addTransition(free, double_free, free_func_rule);

    // Create rule for NULL Store Transition (i.e. when the value is nulled)
    auto store_any_rule = fitx::StoreValueTransitionRule(
        fitx::StoreValueTransitionRule::ANY);

    manager.addTransition(free, init, store_any_rule);
    addStateManager(manager);
  }
};

class UseAfterFreeDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    // Create States
    auto init_args = fitx::StateArgs("init", fitx::StateType::INIT);
    fitx::State &init = manager.createState(init_args);

    auto store_args = fitx::StateArgs("free");
    fitx::State &free = manager.createState(store_args);

    auto free_func_rule = fitx::FunctionArgTransitionRule(free_funcs);
    manager.addTransition(init, free, free_func_rule);

    auto ubi_args = fitx::StateArgs("Used", fitx::StateType::BUG);
    fitx::State &uaf = manager.createState(ubi_args);

    auto use_rule = fitx::UseValueTransitionRule();
    manager.addTransition(free, uaf, use_rule);

    auto store_any_rule = fitx::StoreValueTransitionRule(
        fitx::StoreValueTransitionRule::ANY);

    manager.addTransition(free, init, store_any_rule);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new DoubleFreeDetector(), new UseAfterFreeDetector()};

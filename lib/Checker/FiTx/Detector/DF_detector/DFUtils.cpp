// Double-free typestate: init -> free -> DF (bug). Transitions: Fun Arg (kfree),
// Store ANY (e.g. malloc result stores over freed ptr). Paper Figure 3, Table 5.
#include "Checker/FiTx/Detector/DF_Detector.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Frontend/PropagationConstraint.h"
#include "Checker/FiTx/Frontend/State.h"

namespace DoubleFree {

/// Optional: skip propagating on refcount-like calls (e.g. *put) to reduce FPs.
class OneshotCallConstraint : public fitx::StatefulConstraint {
 public:
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

    if (visited_funcs_.find(parent) == visited_funcs_.end()) {
      visited_funcs_[parent] =
          std::map<std::shared_ptr<fitx::Function>,
                   std::vector<std::shared_ptr<fitx::CallInst>>>();
    }

    if (visited_funcs_[parent].find(called_func) ==
        visited_funcs_[parent].end()) {
      visited_funcs_[parent][called_func] =
          std::vector<std::shared_ptr<fitx::CallInst>>();
    }

    const auto& visited_insts = visited_funcs_[parent][called_func];
    // Bug fix: previously used *stored < *inst (source-location ordering) as a
    // "visited" check, which suppressed any call site that came after an
    // earlier call to the same function — causing false negatives for
    // double-free across multiple call sites. Use pointer identity instead.
    if (std::find_if(visited_insts.begin(), visited_insts.end(),
                     [inst](auto stored) {
                       return stored == inst;
                     }) != visited_insts.end()) {
      return false;
    }

    visited_funcs_[parent][called_func].push_back(inst);
    return true;
  }

 private:
  std::map<std::shared_ptr<fitx::Function>,
           std::map<std::shared_ptr<fitx::Function>,
                    std::vector<std::shared_ptr<fitx::CallInst>>>>
      visited_funcs_;
};

/// Define typestate FSM: init, free, DF (bug). Register transitions: call kfree
/// (init->free, free->DF), store anything (free->init). Paper Figure 3, Table 5.
void define_states(fitx::StateManager& manager) {
  fitx::State& init = manager.getInitState();

  // States: init (default), free (after kfree), DF = double free (bug).
  auto free_args = fitx::StateArgs("free");
  fitx::State& free = manager.createState(free_args);

  auto df_args = fitx::StateArgs("double free", fitx::StateType::BUG);
  fitx::State& double_free = manager.createState(df_args);

  // Create rule for Function Arg Transition (i.e. when the value is passed
  // as the argument of specified functions)
  auto free_func_rule = fitx::FunctionArgTransitionRule(free_funcs);

  manager.addTransition(init, free, free_func_rule);
  manager.addTransition(free, double_free, free_func_rule);

  // Create rule for NULL Store Transition (i.e. when the value is nulled)
  auto store_any_rule = fitx::StoreValueTransitionRule(
      fitx::StoreValueTransitionRule::ANY);
  store_any_rule.setConsiderNullBranch(false);

  manager.addTransition(free, init, store_any_rule);

  manager.enableStatefulConstraint(std::make_shared<OneshotCallConstraint>());
}
}  // namespace DoubleFree

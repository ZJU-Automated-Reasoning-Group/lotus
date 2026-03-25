// #include "llvm/IR/BasicBlock.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/Analysis/LoopInfo.h"
// #include "llvm/IR/Argument.h"
// #include "llvm/IR/CFG.h"
// #include "llvm/IR/Constant.h"
// #include "llvm/IR/DataLayout.h"
// #include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
// #include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
// #include "llvm/IR/InstrTypes.h"
// #include "llvm/IR/Instruction.h"
// #include "llvm/IR/Instructions.h"
// #include "llvm/IR/LegacyPassManager.h"
// #include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
// #include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
// #include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Instructions/BranchInstruction.h"
#include "Checker/FiTx/Core/Value.h"
// #include "Checker/FiTx/Frontend/Utils.h"

// include STL
#include "Checker/FiTx/Core/Casting.h"
// #include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Frontend/BasicBlock.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>

namespace fitx {
BasicBlockInformation::BasicBlockInformation(
    std::shared_ptr<fitx::BasicBlock> basic_block,
    const std::set<State> &states)
    : basic_block_(basic_block), is_partial_states_(false),
      predecessor_partial_(false), states_(states),
      arg_value_states_(0, states), status_(NONE) {
  uint64_t arg_size = 0;
  if (auto function = basic_block_->Parent().lock()) {
    arg_size = function->ArgSize();

    auto return_assignment = function->getReturnAssignments();
    if (return_assignment.find(basic_block_) != return_assignment.end()) {
      return_values_.insert(return_assignment[basic_block_]);
    }
  }
  arg_value_states_ = ArgValueStates(arg_size, states);
}

BasicBlockInformation::BasicBlockInformation(const BasicBlockInformation &info)
    : basic_block_(info.basic_block_), value_states_(info.value_states_),
      arg_value_states_(info.arg_value_states_),
      return_values_(info.return_values_), alias_info_(info.alias_info_),
      pending_values_(info.pending_values_), states_(info.states_),
      is_partial_states_(info.is_partial_states_),
      predecessor_partial_(info.predecessor_partial_),
      time_to_live_(info.time_to_live_), status_(info.status_),
      same_line_predecessors_(info.same_line_predecessors_) {}

// Apply transitions to value (and optionally its alias set). Caller (Analyzer)
// already expands to related + aliased values before calling; here we only
// apply to the single value (per-block alias expansion is done in Analyzer).
bool BasicBlockInformation::changeValueState(
    std::vector<Transition> &transitions,
    std::shared_ptr<fitx::Value> value,
    std::shared_ptr<fitx::Instruction> instruction) {
  bool changed = false;
  std::vector<std::shared_ptr<fitx::Value>> aliased_value;
  aliased_value.push_back(value);

  /* if (auto collection = getAliasInfoForValue(value)) */
  /*   aliased_value.insert(aliased_value.end(), collection->Values().begin(),
   */
  /*                        collection->Values().end()); */

  for (auto alias : aliased_value) {
    if (fitx::shared_isa<Argument>(alias)) {
      bool pending_changed = false;
      if (!pending_values_.empty()) {
        for (auto &pending_values : pending_values_) {
          pending_changed |= pending_values.second.arg_states.transitionState(
              transitions, alias, instruction);
        }
      }
      changed |= pending_changed || arg_value_states_.transitionState(
                                        transitions, alias, instruction);
    } else
      changed |= value_states_.transitionState(transitions, alias, instruction);
  }
  return changed;
}

bool BasicBlockInformation::valueHasState(std::shared_ptr<Value> value) {
  return value_states_.valueExists(value);
}

void BasicBlockInformation::removeValueFromState(
    std::shared_ptr<Value> value,
    std::shared_ptr<fitx::Instruction> instruction) {
  if (!value_states_.valueExists(value))
    return;

  auto null_transition =
      Transition(const_cast<State &>(value_states_.getState(value)),
                 NullState::GetInstance());
  value_states_.setValueState(value, null_transition, instruction);
}

void BasicBlockInformation::resetValueState(
    std::shared_ptr<Value> value,
    std::shared_ptr<fitx::Instruction> instruction) {
  if (!value_states_.valueExists(value))
    return;
  Transition log = value_states_.getTransitionLog(value).ReducedTransition();
  Transition back_init = Transition(log.Target(), log.Source());

  value_states_.setValueState(value, back_init, instruction);
}

void BasicBlockInformation::setPendingValueStates(
    std::weak_ptr<fitx::BasicBlock> basic_block,
    fitx::ArgValueStates arg_value_state) {
  if (pending_values_.find(basic_block) == pending_values_.end()) {
    pending_values_[basic_block] = {
        ArgValueStates(arg_value_state.Size(), states_)};
  }
  pending_values_[basic_block].arg_states.addArgValueState(arg_value_state);
}

std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
BasicBlockInformation::getValueTransitionStates(const State &state) {
  auto value_transition_states = value_states_.getValueTransitionStates(state);
  if (state.EarlyNotification()) {
    auto arg_transition_states =
        arg_value_states_.getValueTransitionStates(state);

    value_transition_states.insert(value_transition_states.end(),
                                   arg_transition_states.begin(),
                                   arg_transition_states.end());
  }
  return value_transition_states;
}

void BasicBlockInformation::setPendingReturnValues(
    std::weak_ptr<fitx::BasicBlock> basic_block,
    std::shared_ptr<fitx::ConstValue> arg_value_state) {
  pending_values_[basic_block].return_values.insert(arg_value_state);
}

std::pair<fitx::BasicBlockValueStates, fitx::ArgValueStates>
BasicBlockInformation::ValueStatesForSuccessor(
    std::shared_ptr<fitx::BasicBlock> successor) {
  std::pair<fitx::BasicBlockValueStates, fitx::ArgValueStates> states(
      value_states_, arg_value_states_);

  if (pending_values_.find(successor) == pending_values_.end()) {
    return states;
  }

  std::shared_ptr<fitx::CallInst> call_inst =
      fitx::shared_dyn_cast<fitx::CallInst>(
          basic_block_->getBranchInst()->Condition());

  if (auto compare_inst = fitx::shared_dyn_cast<fitx::CompareInst>(
          basic_block_->getBranchInst()->Condition())) {
    auto operand = std::find_if(
        compare_inst->Operands().begin(), compare_inst->Operands().end(),
        [](auto operand) { return fitx::shared_isa<CallInst>(operand); });
    // Bug fix: guard against end() before dereferencing.
    if (operand == compare_inst->Operands().end())
      return states;
    call_inst = fitx::shared_dyn_cast<CallInst>(*operand);
  }

  if (!call_inst)
    return states;

  auto operands = call_inst->Arguments();
  for (int i = 0; i < operands.size(); i++) {
    auto operand = operands[i];
    for (auto value :
         pending_values_[successor].arg_states.getValueStateForArg(i)) {
      auto new_value = Value::CreateAppend(operand, value.first);
      if (!fitx::shared_isa<Argument>(new_value))
        states.first.transitionState(value.second, new_value, call_inst);
      else
        states.second.transitionState(value.second, new_value, call_inst);
    }
  }

  return states;
}

std::set<std::shared_ptr<fitx::Value>>
BasicBlockInformation::ReturnCodeForSuccessor(
    std::shared_ptr<fitx::BasicBlock> successor) {
  std::set<std::shared_ptr<fitx::Value>> return_values = return_values_;
  auto branch_inst = basic_block_->getBranchInst();
  if (!branch_inst || !branch_inst->Condition())
    return return_values;

  auto compare_inst = fitx::shared_dyn_cast<fitx::CompareInst>(
      branch_inst->Condition());
  if (!compare_inst)
    return return_values;
  if (pending_values_.find(successor) != pending_values_.end()) {
    return_values.insert(pending_values_[successor].return_values.begin(),
                         pending_values_[successor].return_values.end());

    auto operand = std::find_if(
        compare_inst->Operands().begin(), compare_inst->Operands().end(),
        [](auto operand) { return fitx::shared_isa<CallInst>(operand); });

    // Bug fix: guard against end() before dereferencing.
    if (operand != compare_inst->Operands().end())
      return_values.erase(*operand);
  }

  /* for (auto return_value : return_values) { */
  /*   generateWarning(compare_inst.get(), "Return Value"); */
  /*   if (auto const_int =
   * shared_dyn_cast<fitx::ConstValue>(return_value)) */
  /*     llvm::errs() << const_int->getConstValue() << "(From Pred)\n"; */
  /* } */
  return return_values;
}

BasicBlockValueStates::BasicBlockValueStates(
    const BasicBlockValueStates &states)
    : value_states_(states.value_states_) {}

bool BasicBlockValueStates::operator==(const BasicBlockValueStates &states) {
  return value_states_ == states.value_states_;
}

bool BasicBlockValueStates::valueExists(
    std::shared_ptr<fitx::Value> value) {
  return value_states_.find(value) != value_states_.end();
}

void BasicBlockValueStates::setValueState(
    std::shared_ptr<fitx::Value> value, fitx::Transition &transition,
    std::shared_ptr<fitx::Instruction> instruction) {
  value_states_[value].addTransition(transition, instruction);
}

void BasicBlockValueStates::setValueState(
    std::shared_ptr<fitx::Value> value, fitx::TransitionLogs &logs) {
  value_states_[value] = logs;
}

TransitionLogs &BasicBlockValueStates::getTransitionLog(
    std::shared_ptr<fitx::Value> value) {
  return value_states_[value];
}

std::vector<std::shared_ptr<fitx::Value>>
BasicBlockValueStates::getStateValues(const fitx::State &state) {
  std::vector<std::shared_ptr<fitx::Value>> values;
  for (auto value = value_states_.begin(); value != value_states_.end();
       value++) {
    if (value->second.CurrentState() == state)
      values.push_back(value->first);
  }
  return values;
}

std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
BasicBlockValueStates::getValueTransitionStates(const fitx::State &state) {
  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
      values;
  for (auto value = value_states_.begin(); value != value_states_.end();
       value++) {
    if (value->second.CurrentState() == state)
      values.push_back(std::make_pair(value->first, &(value->second)));
  }
  return values;
}

bool BasicBlockValueStates::transitionState(
    std::vector<Transition> &transitions,
    std::shared_ptr<fitx::Value> value,
    std::shared_ptr<fitx::Instruction> instruction) {
  generateWarning(instruction.get(), "Transition State Called");
  if (valueExists(value)) {
    generateWarning(instruction.get(), "value exists");
    const State &current_state = getState(value);
    const TransitionLogs &current_transitions = value_states_[value];

    // Search for possible transitions
    auto next_transition =
        std::find_if(transitions.begin(), transitions.end(),
                     [&current_state](Transition transition) {
                       return transition.Source() == current_state;
                     });

    if (next_transition != transitions.end() &&
        current_transitions.CurrentInstruction() != instruction &&
        *current_transitions.CurrentInstruction() <= *instruction) {
      setValueState(value, *next_transition, instruction);
      return true;
    }
    return false;
  }

  fitx::Transition *transition = nullptr;
  auto init_transition = transitions.begin();

  while (init_transition != transitions.end()) {
    if (init_transition->Source().isInitState() &&
        (!transition || init_transition->Target() < transition->Target()))
      transition = &(*init_transition);
    init_transition = std::find_if(init_transition + 1, transitions.end(),
                                   [](Transition transition) {
                                     return transition.Source().isInitState();
                                   });
  }

  if (transition) {
    setValueState(value, *transition, instruction);
    return true;
  }
  return false;
}

void BasicBlockValueStates::print() {
  for (auto states : value_states_) {
    llvm::errs() << states.first << " " << states.second.ReducedTransition()
                 << "\n";
  }
}

/* ArgTransitions Class */
ArgTransitions::ArgTransitions() : transition_per_state_() {}

ArgTransitions::ArgTransitions(std::set<fitx::State> states) {
  for (auto state : states)
    transition_per_state_[state] = TransitionLogs();
}

ArgTransitions::ArgTransitions(const ArgTransitions &arg_transitions)
    : transition_per_state_(arg_transitions.transition_per_state_) {}

bool ArgTransitions::operator==(const ArgTransitions &arg_transitions) const {
  return transition_per_state_ == arg_transitions.transition_per_state_;
}

void ArgTransitions::addArgTransitions(const ArgTransitions &arg_transitions) {
  for (auto state : arg_transitions.transition_per_state_) {
    if (state.second.isDummy())
      continue;

    TransitionLogs &current_logs = transition_per_state_[state.first];
    if (current_logs.isDummy() ||
        state.second.CurrentState() < current_logs.CurrentState()) {
      transition_per_state_[state.first] = state.second;
    }
  }
}

bool ArgTransitions::addTransition(
    std::vector<Transition> &transitions,
    std::shared_ptr<fitx::Instruction> inst) {
  bool changed = false;
  for (auto &state : transition_per_state_) {
    State current_state = state.first;
    if (!state.second.isDummy())
      current_state = state.second.CurrentState();

    // Search for possible transitions
    auto next_transition =
        std::find_if(transitions.begin(), transitions.end(),
                     [&current_state](Transition transition) {
                       return transition.Source() == current_state;
                     });

    if (next_transition != transitions.end()) {
      state.second.addTransition(*next_transition, inst);
      changed = true;
    }
  }
  return changed;
}

TransitionLogs ArgTransitions::getTransitionLog(State state) {
  return transition_per_state_[state];
}

TransitionLogs getTransitionLog(State state);

// ArgValueStates Class
ArgValueStates::ArgValueStates() : value_states_(0) {}
/* ArgValueStates::ArgValueStates(uint64_t arg_num) : value_states_(arg_num) {}
 */
ArgValueStates::ArgValueStates(uint64_t arg_num, const std::set<State> &states)
    : value_states_(arg_num), states_(states) {}

ArgValueStates::ArgValueStates(const ArgValueStates &arg_value_states)
    : states_(arg_value_states.states_) {
  value_states_ = arg_value_states.value_states_;
}

ArgValueStates &
ArgValueStates::operator=(const ArgValueStates &arg_value_states) {
  value_states_ = arg_value_states.value_states_;
  return *this;
}

const std::map<std::shared_ptr<fitx::Value>, std::vector<Transition>>
ArgValueStates::getValueStateForArg(int64_t index) const {
  std::map<std::shared_ptr<fitx::Value>, std::vector<Transition>> new_map;

  if (value_states_.size() <= index)
    return new_map;
  for (auto value : value_states_[index]) {
    for (auto &transition_log : value.second.TransitionPerState()) {
      if (!transition_log.second.isDummy() &&
          transition_log.second.ReducedTransition().Source() !=
              transition_log.second.ReducedTransition().Target()) {
        new_map[value.first].push_back(
            transition_log.second.ReducedTransition());
      }
    }
  }

  return new_map;
}

/* const std::map<std::shared_ptr<fitx::Value>,
 * std::vector<TransitionLogs>> */
/* ArgValueStates::getValueTransitionLogsForArg(int64_t index) const { */
/*   if (value_states_.size() <= index) */
/*     return std::map<std::shared_ptr<fitx::Value>, */
/*                     std::vector<TransitionLogs>>(); */

/*   return value_states_[index]; */
/* } */

const std::map<std::shared_ptr<fitx::Value>, ArgTransitions>
ArgValueStates::getArgTransitions(int64_t index) const {
  if (value_states_.size() <= index)
    return std::map<std::shared_ptr<fitx::Value>, ArgTransitions>();
  return value_states_[index];
}

std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
ArgValueStates::getValueTransitionStates(const fitx::State &state) {
  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
      values;
  for (auto &value_states : value_states_) {
    for (auto value = value_states.begin(); value != value_states.end();
         value++) {
      auto &transition_per_state = value->second.TransitionPerState();
      for (auto &transition_logs : transition_per_state) {
        if (transition_logs.second.isDummy())
          continue;
        if (transition_logs.second.LeastSignificantSource().isInitState()) {
          if (transition_logs.second.MostSignificantTarget() == state) {
            values.push_back(
                std::make_pair(value->first, &(transition_logs.second)));
          }
        }
      }
      /* std::vector<TransitionLogs*> new_logs; */
      /* for (fitx::TransitionLogs& log : value->second) { */
      /*   if (log.LeastSignificantSource().isInitState()) { */
      /*     if (log.MostSignificantTarget() == state) { */
      /*       new_logs.push_back(&log); */
      /*     } */
      /*   } */
      /* } */

      /* for (auto log : new_logs) */
      /*   values.push_back(std::make_pair(value->first, log)); */
    }
  }
  return values;
}

bool ArgValueStates::ValueExistsInArg(uint64_t arg,
                                      std::shared_ptr<Value> value) {
  if (value_states_.size() <= arg)
    return false;
  return value_states_[arg].find(value) != value_states_[arg].end();
}

void ArgValueStates::addArgValueState(const ArgValueStates &states) {
  for (auto arg_idx = 0; arg_idx < std::min(value_states_.size(),
                                            static_cast<size_t>(states.Size()));
       arg_idx++) {
    for (auto &value_transitions : states.getArgTransitions(arg_idx)) {
      const std::shared_ptr<Value> value = value_transitions.first;
      ArgTransitions arg_transitions = value_transitions.second;

      if (ValueExistsInArg(arg_idx, value))
        value_states_[arg_idx][value].addArgTransitions(arg_transitions);
      else
        value_states_[arg_idx][value] = ArgTransitions(arg_transitions);

      /* for (auto trans: value_states_[arg_idx][value].TransitionPerState()) {
       */
      /*   if (trans.second.isDummy()) */
      /*     continue; */
      /*   llvm::errs() << trans.first.Name() << " (First State)\n"; */
      /*   llvm::errs() << trans.second.ReducedTransition() << "(Transition)\n";
       */
      /* } */
    }
    /* for (auto value_transitions : */
    /*      states.getValueTransitionLogsForArg(arg_idx)) { */
    /*   auto& transition_logs = value_transitions.second; */
    /*   auto& current_transitions = */
    /*       value_states_[arg_idx][value_transitions.first]; */

    /*   for (auto transition_log : transition_logs) { */
    /*     auto found_transition = */
    /*         std::find_if(current_transitions.begin(),
     * current_transitions.end(), */
    /*                      [transition_log](auto current_log) { */
    /*                        return current_log.ReducedTransition().Source() ==
     */
    /*                               transition_log.ReducedTransition().Source();
     */
    /*                      }); */

    /*     if (found_transition == current_transitions.end()) { */
    /*       current_transitions.push_back(transition_log); */
    /*     } else if (transition_log.CurrentState() < */
    /*                found_transition->CurrentState()) { */
    /*       int ind = found_transition - current_transitions.begin(); */
    /*       current_transitions[ind] = transition_log; */
    /*     } */
    /*   } */
    /* } */
  }
}

bool ArgValueStates::transitionState(
    std::vector<Transition> &transitions,
    std::shared_ptr<fitx::Value> value,
    std::shared_ptr<fitx::Instruction> instruction) {
  if (auto argument = fitx::shared_dyn_cast<fitx::Argument>(value)) {
    uint64_t arg_index = argument->ArgNum();
    if (value_states_.size() <= arg_index)
      return false;

    if (!ValueExistsInArg(arg_index, value))
      value_states_[arg_index][value] = ArgTransitions(states_);

    return value_states_[arg_index][value].addTransition(transitions,
                                                         instruction);

    /* std::set<int> updated_logs; */
    /* std::vector<TransitionLogs> new_logs; */
    /* auto& states = value_states_[arg_index][value]; */
    /* for (auto transition : transitions) { */
    /*   bool source_exists = false; */
    /*   for (int i = 0; i < states.size(); i++) { */
    /*     if (updated_logs.find(i) != updated_logs.end()) continue; */
    /*     if (states[i].CurrentState() == transition.Source()) { */
    /*       states[i].addTransition(transition, instruction); */
    /*       updated_logs.insert(i); */
    /*     } */

    /*     if (states[i].ReducedTransition().Source() == transition.Source()) {
     */
    /*       source_exists = true; */
    /*     } */
    /*   } */

    /*   if (!source_exists) */
    /*     new_logs.push_back(TransitionLogs(transition, instruction)); */
    /* } */

    /* states.insert(states.end(), new_logs.begin(), new_logs.end()); */
  }
  return true;
}

void ArgValueStates::print() {
  for (auto arg : value_states_) {
    for (auto value : arg) {
      llvm::errs() << value.first << "\n";
      for (auto states : value.second.TransitionPerState()) {
        if (!states.second.isDummy())
          llvm::errs() << states.second.ReducedTransition() << "\n";
      }
    }
  }
}

bool ArgValueStates::operator==(const ArgValueStates &states) {
  return value_states_ == states.value_states_;
}

bool BasicBlockInformation::operator==(
    const fitx::BasicBlockInformation &prev_block_info) {
  return value_states_ == prev_block_info.value_states_ &&
         arg_value_states_ == prev_block_info.arg_value_states_ &&
         return_values_ == prev_block_info.return_values_;
}

void BasicBlockInformation::addReturnValues(
    const std::set<std::shared_ptr<fitx::Value>> &return_values) {
  return_values_.insert(return_values.begin(), return_values.end());
}

void BasicBlockInformation::addReturnValue(
    std::shared_ptr<fitx::Value> value) {
  return_values_.insert(value);
}

bool BasicBlockInformation::ReturnValueSatisfiable(long value) {
  switch (getBlockStatus()) {
  case BasicBlockInformation::ERROR:
    if (!value)
      return false;
    break;
  case BasicBlockInformation::SUCCESS:
    if (value)
      return false;
    break;
  default:
    break;
  }
  return true;
}

void BasicBlockInformation::removeReturnvalue(int value) {
  auto found_value = std::find_if(
      return_values_.begin(), return_values_.end(), [value](auto ret_val) {
        if (auto const_ret = fitx::shared_dyn_cast<ConstValue>(ret_val))
          return value == const_ret->getConstValue();
        return false;
      });
  // Bug fix: guard against end() before dereferencing.
  if (found_value != return_values_.end())
    return_values_.erase(found_value);
}

void BasicBlockInformation::setPartialStates(bool partial_states) {
  is_partial_states_ = partial_states;
}

bool BasicBlockInformation::IsInSamelinePredecessor(
    std::shared_ptr<fitx::BasicBlock> block) {
  return std::find_if(same_line_predecessors_.begin(),
                      same_line_predecessors_.end(), [block](auto predecessor) {
                        return block == predecessor.lock();
                      }) != same_line_predecessors_.end();
}

void BasicBlockInformation::addSameLinePredecessor(
    std::shared_ptr<fitx::BasicBlock> block) {
  same_line_predecessors_.push_back(block);
}

void BasicBlockInformation::addSameLinePredecessor(
    std::vector<std::weak_ptr<fitx::BasicBlock>> blocks) {
  same_line_predecessors_.insert(same_line_predecessors_.end(), blocks.begin(),
                                 blocks.end());
}

void BasicBlockInformation::readCurrentStates() {
  generateWarning(basic_block_.get(), "--- Local States ---");
  value_states_.print();

  generateWarning(basic_block_.get(), "--- Arg States ---");
  arg_value_states_.print();
}

} // namespace fitx

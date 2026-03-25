#include "llvm/ADT/APFloat.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

// include STL
#include "Checker/FiTx/Frontend/PropagationConstraint.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/FiTx/Frontend/StateTransition.h"

#include <algorithm>
#include <cassert>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <vector>

namespace fitx {

/* State Class */
State::State(int ID, std::string name, StateType type, StateMergeMethod method,
             bool early_notification, TriggerConstraint constraint,
             BugNotificationTiming timing)
    : ID_(ID), name_(name), type_(type), method_(method),
      early_notification_(early_notification), trigger_constraint_(constraint),
      timing_(timing) {};

State::State(int ID, StateArgs &args)
    : ID_(ID), name_(args.name_), type_(args.type_), method_(args.method_),
      early_notification_(args.early_notification_),
      trigger_constraint_(args.trigger_constraint_), timing_(args.timing_) {};

State::State(const State &state)
    : ID_(state.ID_), name_(state.name_), type_(state.type_),
      method_(state.method_), early_notification_(state.early_notification_),
      trigger_constraint_(state.trigger_constraint_), timing_(state.timing_) {};

void State::addTransition(State &state, TransitionRule &rule) {
  transitions_.push_back(std::pair<State, TransitionRule>(state, rule));
}

bool State::operator<(const State &state) const { return ID_ < state.ID_; }

bool State::operator<(const StateArgs &args) const {
  if (type_ == args.type_)
    return name_ < args.name_;

  return type_ < args.type_;
}

bool State::operator==(const State &state) const { return ID_ == state.ID_; }
bool State::operator!=(const State &state) const { return ID_ != state.ID_; }

bool State::operator==(const std::string &name) const { return name == name_; }
bool State::operator==(const StateArgs &args) const {
  return name_ == args.name_ && type_ == args.type_;
}

State &State::operator=(const State &state) {
  ID_ = state.ID_;
  name_ = state.name_;
  type_ = state.type_;
  method_ = state.method_;
  timing_ = state.timing_;
  trigger_constraint_ = state.trigger_constraint_;
  early_notification_ = state.early_notification_;
  return *this;
}

StateManager::StateManager()
    : early_state_transition_(false), init_state_(nullptr),
      propagation_constraint_(nullptr) {
  StateArgs init_args =
      fitx::StateArgs("init", fitx::StateType::INIT);
  init_state_ = &createState(init_args);

  transition_manager_ = std::make_shared<StateTransitionManager>();
}

/* StateManager Class */
StateManager::StateManager(std::set<State> states)
    : states_(states), early_state_transition_(false) {
  StateArgs init_args =
      fitx::StateArgs("init", fitx::StateType::INIT);
  init_state_ = &createState(init_args);

  transition_manager_ = std::make_shared<StateTransitionManager>();
}

State &StateManager::createState(StateArgs &args) {
  auto searched_states = std::find(states_.begin(), states_.end(), args);
  if (searched_states != states_.end())
    return const_cast<State &>(*searched_states);

  int id = args.type_ == BUG ? State::kBugStateIDBase + states_.size()
                             : states_.size();
  State new_state = State(id, args);
  auto inserted_state = states_.insert(new_state);
  return const_cast<State &>(*inserted_state.first);
}

StateManager StateManager::createWithInitStates(std::vector<StateArgs> &args) {
  StateManager manager;
  for (auto arg : args) {
    manager.createState(arg);
  }
  return manager;
}

std::set<State> StateManager::createStates(std::vector<StateArgs> &states) {
  std::set<State> new_states;
  for (auto state : states) {
    State new_state = createState(state);
    new_states.insert(new_state);
  }
  return new_states;
}

State &StateManager::getByName(const std::string &name) {
  auto state =
      std::find_if(states_.begin(), states_.end(),
                   [name](const State &t) -> bool { return name == t.Name(); });
  if (state == states_.end())
    throw std::out_of_range("Could not find the state");
  return const_cast<State &>(*state);
}

std::shared_ptr<fitx::StateTransitionManager>
StateManager::TransitionManager() {
  return transition_manager_;
}

void StateManager::addTransition(State &source, State &target,
                                 TransitionRule &rule) {
  fitx::Transition transition =
      transition_manager_->createTransition(source, target);

  transition_manager_->addTransitionRule(transition, rule);
}

const std::set<State> &StateManager::getStates() { return states_; }
const std::set<State> StateManager::getBugStates() {
  if (!bug_states_.empty())
    return bug_states_;

  for (auto &state : states_) {
    if (state.isBugState())
      bug_states_.insert(state);
  }
  return bug_states_;
}

State &StateManager::getInitState() { return *init_state_; }

void StateManager::setEarlyStateTransition(bool early_transition) {
  early_state_transition_ = early_transition;
}

void StateManager::enableStatefulConstraint(
    std::shared_ptr<StatefulConstraint> constraint) {
  propagation_constraint_ = constraint;
}

Transition::Transition(const State source, const State target)
    : source_(source), target_(target) {}

Transition::Transition(const Transition &transition)
    : source_(transition.source_), target_(transition.target_) {}

void Transition::addTransitionRule(std::weak_ptr<TransitionRule> arg) {
  transition_args_.push_back(std::weak_ptr<TransitionRule>(arg));
  return;
}

Transition &Transition::operator=(const Transition &transition) {
  source_ = transition.source_;
  target_ = transition.target_;
  return *this;
}

bool Transition::operator<(const Transition &transition) const {
  // Bug fix: must compare both source and target so that transitions with the
  // same target but different sources are not treated as equal in std::set.
  if (source_ == transition.source_)
    return target_ < transition.target_;
  return source_ < transition.source_;
}

bool Transition::operator==(const Transition &transition) const {
  return source_ == transition.source_ && target_ == transition.target_;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                              const fitx::Transition &transition) {
  ostream << transition.Source().Name() << " to " << transition.Target().Name();
  return ostream;
}

TransitionLogs::TransitionLogs() : transition_logs_(), warned_(false) {}

TransitionLogs::TransitionLogs(const TransitionLogs &logs)
    : transition_logs_(logs.transition_logs_) {
  least_significant_source_ = logs.least_significant_source_;
  most_significant_target_ = logs.most_significant_target_;
}

TransitionLogs::TransitionLogs(
    Transition transition, std::shared_ptr<fitx::Instruction> instruction)
    : transition_logs_{Log{transition, instruction}} {
  least_significant_source_ = transition.Source();
  most_significant_target_ = transition.Target();
}

const State &TransitionLogs::CurrentState() const {
  assert(!transition_logs_.empty());

  return transition_logs_.back().transition.Target();
}

void TransitionLogs::addTransition(
    fitx::Transition &transition,
    std::shared_ptr<fitx::Instruction> instruction) {
  transition_logs_.push_back(Log{transition, instruction});

  if (!least_significant_source_ ||
      transition.Source() < *least_significant_source_)
    least_significant_source_ = transition.Source();

  if (!most_significant_target_ ||
      *most_significant_target_ < transition.Target())
    most_significant_target_ = transition.Target();
}

void TransitionLogs::setWarned() { warned_ = true; }

void TransitionLogs::generateLog(llvm::raw_ostream &stream) const {
  for (auto log : transition_logs_) {
    auto transition = log.transition;
    fitx::generateLog(stream, log.instruction.get(),
                           "[Transition] " + transition.Source().Name() +
                               " to " + transition.Target().Name());
  }
}

void TransitionLogs::logicalTerminate(
    std::shared_ptr<fitx::Instruction> instruction) {
  auto null_transition = Transition(transition_logs_.back().transition.Target(),
                                    NullState::GetInstance());
  addTransition(null_transition, instruction);
}

Transition TransitionLogs::ReducedTransition() const {
  return Transition(transition_logs_.front().transition.Source(),
                    transition_logs_.back().transition.Target());
}

bool TransitionLogs::operator==(const TransitionLogs &logs) const {
  return transition_logs_ == logs.transition_logs_;
}

TransitionLogs &TransitionLogs::operator=(const TransitionLogs &logs) {
  transition_logs_ = std::vector<Log>(logs.transition_logs_);
  least_significant_source_ = logs.least_significant_source_;
  most_significant_target_ = logs.most_significant_target_;
  return *this;
}

Transition StateTransitionManager::createTransition(State &source,
                                                    State &target) {
  return Transition(source, target);
}

void StateTransitionManager::addTransitionRule(Transition &transition,
                                               TransitionRule &arg) {
  std::shared_ptr<TransitionRule> rule;
  switch (arg.Trigger()) {
  case TransitionTrigger::FUNCTION_ARG: {
    auto function_arg_rule = std::make_shared<FunctionArgTransitionRule>(
        *static_cast<FunctionArgTransitionRule *>(&arg));
    registerFunctionArgTransition(function_arg_rule->FunctionArgs(),
                                  transition);
    rule = function_arg_rule;
    break;
  }
  case TransitionTrigger::STORE_VALUE: {
    auto store_value_rule = std::make_shared<StoreValueTransitionRule>(
        *static_cast<StoreValueTransitionRule *>(&arg));
    registerStoreTransition(store_value_rule, transition);
    rule = store_value_rule;
    break;
  }
  case TransitionTrigger::USE_VALUE: {
    auto use_value_transition = std::make_shared<UseValueTransitionRule>(
        *static_cast<UseValueTransitionRule *>(&arg));
    registerUseTransition(use_value_transition, transition);
    rule = use_value_transition;
    break;
  }
  case TransitionTrigger::ALIASED_VALUE: {
    // Alias transitions: applied when ptr = value_operand (store); propagates
    // typestate to all may-aliased values (paper Table 5: Alias).
    auto alias_value_transition = std::make_shared<AliasValueTransitionRule>(
        *static_cast<AliasValueTransitionRule *>(&arg));
    registerAliasTransition(alias_value_transition, transition);
    rule = alias_value_transition;
    break;
  }
  }

  rules_[rule->Trigger()].push_back(rule);
  transition.addTransitionRule(rule);
  return;
}

void StateTransitionManager::registerFunctionArgTransition(
    const FunctionArgTransitionRule::FunctionArg &arg,
    fitx::Transition transition) {
  if (!existsInFunctionArgTransition(arg))
    function_transitions_[arg] = std::vector<fitx::Transition>();
  function_transitions_[arg].push_back(transition);
}

void StateTransitionManager::registerFunctionArgTransition(
    const std::vector<FunctionArgTransitionRule::FunctionArg> &args,
    fitx::Transition transition) {
  for (auto arg : args) {
    registerFunctionArgTransition(arg, transition);
  }
}

void StateTransitionManager::registerStoreTransition(
    std::shared_ptr<StoreValueTransitionRule> rule,
    fitx::Transition transition) {
  if (store_transitions_.find(rule->Type()) == store_transitions_.end())
    store_transitions_[rule->Type()] = std::vector<fitx::Transition>();
  store_transitions_[rule->Type()].push_back(transition);

  if (rule->Type() == fitx::StoreValueTransitionRule::CALL_FUNC) {
    for (auto func : rule->FunctionNames()) {
      if (call_store_transitions_.find(func) == call_store_transitions_.end())
        call_store_transitions_[func] = std::vector<fitx::Transition>();
      call_store_transitions_[func].push_back(transition);
    }
  }

  if (rule->ConsiderNullBranch()) {
    auto new_rule =
        static_cast<fitx::StoreValueTransitionRule::StoreValueType>(
            rule->Type() + 4);
    if (store_transitions_.find(new_rule) == store_transitions_.end())
      store_transitions_[new_rule] = std::vector<fitx::Transition>();
    store_transitions_[new_rule].push_back(transition);
  }
}

void StateTransitionManager::registerUseTransition(
    std::shared_ptr<UseValueTransitionRule> rule,
    fitx::Transition transition) {
  use_transitions_.push_back(transition);
}

void StateTransitionManager::registerAliasTransition(
    std::shared_ptr<AliasValueTransitionRule> rule,
    fitx::Transition transition) {
  alias_transitions_.push_back(transition);
}

bool StateTransitionManager::existsInFunctionArgTransition(
    const FunctionArgTransitionRule::FunctionArg &arg) {
  return function_transitions_.find(arg) != function_transitions_.end();
}

const std::pair<FunctionArgTransitionRule::FunctionArg,
                std::vector<fitx::Transition>>
/* const std::vector<fitx::Transition> */
StateTransitionManager::getFunctionArgTransitions(const std::string &name,
                                                  unsigned int arg_num) {
  FunctionArgTransitionRule::FunctionArg arg(name, arg_num);
  if (existsInFunctionArgTransition(arg)) {
    return *function_transitions_.find(arg);
  }
  return std::make_pair(arg, std::vector<fitx::Transition>());
}

std::vector<fitx::Transition>
StateTransitionManager::getStoreArgTransitions(
    fitx::StoreValueTransitionRule::StoreValueType type,
    const std::string &name) {
  std::vector<fitx::Transition> transitions;
  if (type == fitx::StoreValueTransitionRule::CALL_FUNC) {
    if (call_store_transitions_.find(name) != call_store_transitions_.end())
      return call_store_transitions_[name];
  } else {
    if (store_transitions_.find(type) != store_transitions_.end())
      return store_transitions_[type];
  }
  return std::vector<fitx::Transition>();
}

std::vector<fitx::Transition>
StateTransitionManager::getUseValueTransitions() {
  return use_transitions_;
}

std::vector<fitx::Transition>
StateTransitionManager::getAliasTransitions() {
  return alias_transitions_;
}
}; // namespace fitx

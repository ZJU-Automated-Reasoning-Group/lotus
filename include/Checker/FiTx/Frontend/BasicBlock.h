/// \file BasicBlock.h
/// \brief FiTx per-block analysis state: value typestates, arg summaries,
/// pending (return-code aware) propagation, and may-alias (paper §4.2, 4.3).
///
/// BasicBlockInformation holds value_states_ (value -> TransitionLogs),
/// arg_value_states_ (per-arg summaries for callee application),
/// pending_values_ per successor (for return-code aware merge), and alias_info_
/// (store-based may-alias; not merged across predecessors).
#pragma once
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

#include "Checker/FiTx/Core/Instruction.h"

// include STL
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Frontend/State.h"

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

namespace fitx {

/// Per-state transition history for one value at one argument index; used to
/// build and apply callee summaries (paper §4.3).
/// Tracks which transitions were applied so we can propagate the right state to
/// callers.
class ArgTransitions {
public:
  ArgTransitions();
  ArgTransitions(std::set<fitx::State> states);
  ArgTransitions(const ArgTransitions &arg_transitions);

  bool operator==(const ArgTransitions &arg_transitions) const;

  void addArgTransitions(const ArgTransitions &arg_transitions);
  bool addTransition(std::vector<Transition> &transitions,
                     std::shared_ptr<fitx::Instruction> inst);
  TransitionLogs getTransitionLog(State state);
  std::map<fitx::State, TransitionLogs> &TransitionPerState() {
    return transition_per_state_;
  }

private:
  std::map<fitx::State, TransitionLogs> transition_per_state_;
};

/// Per-argument typestate summary: for each arg index, value -> ArgTransitions
/// (state -> TransitionLogs). Used to apply callee summary at call sites
/// (paper §4.3) and for return-code aware propagation.
class ArgValueStates {
public:
  ArgValueStates();
  ArgValueStates(uint64_t arg_num, const std::set<State> &states);
  ArgValueStates(const ArgValueStates &arg_value_states);

  bool operator==(const ArgValueStates &states);
  ArgValueStates &operator=(const ArgValueStates &arg_value_states);

  bool transitionState(std::vector<Transition> &transitions,
                       std::shared_ptr<fitx::Value> value,
                       std::shared_ptr<fitx::Instruction> instruction);

  void addArgValueState(const ArgValueStates &states);
  const uint64_t Size() const { return value_states_.size(); }
  bool ValueExistsInArg(uint64_t arg, std::shared_ptr<Value>);

  const std::map<std::shared_ptr<fitx::Value>, std::vector<Transition>>
  getValueStateForArg(int64_t index) const;

  /* const std::map<std::shared_ptr<fitx::Value>,
   * std::vector<TransitionLogs>> */
  /* getValueTransitionLogsForArg(int64_t index) const; */

  const std::map<std::shared_ptr<fitx::Value>, ArgTransitions>
  getArgTransitions(int64_t index) const;

  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
  getValueTransitionStates(const fitx::State &state);

  void print();

private:
  /* std::vector< */
  /*     std::map<std::shared_ptr<fitx::Value>,
   * std::vector<TransitionLogs>>> */
  /*     value_states_; */

  std::vector<std::map<std::shared_ptr<fitx::Value>, ArgTransitions>>
      value_states_;

  const std::set<State> states_;
};

/// Per-block map: value -> TransitionLogs (current typestate + transition
/// history). Merged from predecessors in createBasicBlockInfo; updated when
/// applying transitions (paper §4.2).
class BasicBlockValueStates {
public:
  BasicBlockValueStates() = default;
  BasicBlockValueStates(const BasicBlockValueStates &states);
  bool operator==(const BasicBlockValueStates &states);

  bool valueExists(std::shared_ptr<fitx::Value> value);
  void updateReturnValue(std::shared_ptr<fitx::BasicBlock> block);
  bool transitionState(std::vector<Transition> &transitions,
                       std::shared_ptr<fitx::Value> value,
                       std::shared_ptr<fitx::Instruction> instruction);

  void setValueState(std::shared_ptr<fitx::Value> value,
                     fitx::Transition &states,
                     std::shared_ptr<fitx::Instruction> instruction);

  void setValueState(std::shared_ptr<fitx::Value> value,
                     fitx::TransitionLogs &logs);

  TransitionLogs &getTransitionLog(std::shared_ptr<fitx::Value> value);

  std::vector<std::shared_ptr<fitx::Value>>
  getStateValues(const fitx::State &state);

  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
  getValueTransitionStates(const fitx::State &state);

  const State &getState(std::shared_ptr<fitx::Value> value) {
    return value_states_[value].CurrentState();
  };

  const std::map<std::shared_ptr<fitx::Value>, TransitionLogs>
  ValueStates() {
    return value_states_;
  };

  void print();

private:
  std::map<std::shared_ptr<fitx::Value>, TransitionLogs> value_states_;
};

/// All analysis state for one basic block: value_states_ (typestate per value),
/// arg_value_states_ (per-arg summary), pending_values_ (per successor for
/// return-code aware propagation), return_values_, and alias_info_ (store-based
/// may-alias; recorded and used during analysis, not merged across preds).
class BasicBlockInformation {
public:
  constexpr static int kMaxTimeToLive = 5;

  enum BlockStatus { NONE, ERROR, SUCCESS, NUTRAL };

  BasicBlockInformation(std::shared_ptr<fitx::BasicBlock> basic_block,
                        const std::set<State> &states);
  BasicBlockInformation(const BasicBlockInformation &info);

  bool changeValueState(std::vector<Transition> &transitions,
                        std::shared_ptr<fitx::Value> value,
                        std::shared_ptr<fitx::Instruction> instruction);
  bool valueHasState(std::shared_ptr<fitx::Value> value);
  void
  removeValueFromState(std::shared_ptr<fitx::Value> value,
                       std::shared_ptr<fitx::Instruction> instruction);

  void resetValueState(std::shared_ptr<fitx::Value> value,
                       std::shared_ptr<fitx::Instruction> instruction);

  fitx::BasicBlockValueStates &ValueStates() { return value_states_; };

  std::pair<fitx::BasicBlockValueStates, fitx::ArgValueStates>
  ValueStatesForSuccessor(std::shared_ptr<fitx::BasicBlock> successor);

  std::set<std::shared_ptr<fitx::Value>>
  ReturnCodeForSuccessor(std::shared_ptr<fitx::BasicBlock> successor);

  fitx::ArgValueStates &getArgValueStates() { return arg_value_states_; };
  void setPendingValueStates(std::weak_ptr<fitx::BasicBlock>,
                             fitx::ArgValueStates arg_value_state);

  std::vector<std::pair<std::shared_ptr<fitx::Value>, TransitionLogs *>>
  getValueTransitionStates(const State &state);

  void setPendingReturnValues(std::weak_ptr<fitx::BasicBlock>,
                              std::shared_ptr<fitx::ConstValue>);

  bool operator==(const fitx::BasicBlockInformation &prev_block_info);

  // TODO: TTL should be removed once worklist alogrithm is implemented
  // correctly
  void setTimeToLive(int ttl) { time_to_live_ = ttl; }
  int TimeToLive() { return time_to_live_; }

  const std::set<std::shared_ptr<fitx::Value>> &ReturnValues() {
    return return_values_;
  }

  void addReturnValues(
      const std::set<std::shared_ptr<fitx::Value>> &return_values);
  void addReturnValue(std::shared_ptr<fitx::Value> value);

  bool ReturnValueSatisfiable(long value);

  void removeReturnvalue(int value);

  std::shared_ptr<fitx::BasicBlock> BasicBlock() { return basic_block_; };

  void setPartialStates(bool partial_states);
  bool PartialStates() { return is_partial_states_; };

  void setPredecessorPartial(bool partial_states) {
    predecessor_partial_ = partial_states;
  };

  bool PredecessorPartial() { return predecessor_partial_; };

  bool IsInSamelinePredecessor(std::shared_ptr<fitx::BasicBlock> block);
  void addSameLinePredecessor(std::shared_ptr<fitx::BasicBlock> block);

  void addSameLinePredecessor(
      std::vector<std::weak_ptr<fitx::BasicBlock>> blocks);

  std::vector<std::weak_ptr<fitx::BasicBlock>> SameLinePredecessors() {
    return same_line_predecessors_;
  }

  AliasValues &getAliasValues() { return alias_info_; }

  void readCurrentStates();

  void setBlockStatus(BlockStatus status) {
    if (status_ != NUTRAL)
      status_ = status;
  }
  BlockStatus getBlockStatus() { return status_; }

private:
  std::shared_ptr<fitx::BasicBlock> basic_block_;
  /// Return-code aware: for each successor block (branch target), store the
  /// callee arg states and return value to propagate when we enter that
  /// successor.
  struct PendingValues {
    fitx::ArgValueStates arg_states;
    std::set<std::shared_ptr<fitx::ConstValue>> return_values;
  };

  struct ValueStates {
    fitx::ArgValueStates arg_value_states_;
    fitx::BasicBlockValueStates value_states_;
  };

  // TODO: remove for quasi worklist algorithm
  int time_to_live_ = kMaxTimeToLive;
  bool is_partial_states_;
  bool predecessor_partial_;

  std::vector<std::weak_ptr<fitx::BasicBlock>> same_line_predecessors_;

  std::set<std::shared_ptr<fitx::Value>> return_values_;

  AliasValues alias_info_;

  std::map<std::weak_ptr<fitx::BasicBlock>, struct PendingValues,
           std::owner_less<std::weak_ptr<fitx::BasicBlock>>>
      pending_values_;

  fitx::ArgValueStates arg_value_states_;
  fitx::BasicBlockValueStates value_states_;

  std::set<State> states_;

  struct ValueStates refcounted_states_;
  BlockStatus status_;
};

} // namespace fitx

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
#include "Checker/FiTx/Frontend/StateTransition.h"

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
// TransitionRule Class
TransitionRule::TransitionRule(TransitionTrigger trigger) : trigger_(trigger) {}

TransitionRule::TransitionRule(const TransitionRule &rule)
    : trigger_(rule.trigger_) {}
TransitionTrigger TransitionRule::Trigger() { return trigger_; }

// FunctionArgTransitionRule Class
FunctionArgTransitionRule::FunctionArgTransitionRule(
    std::vector<std::string> names)
    : TransitionRule(TransitionTrigger::FUNCTION_ARG) {
  for (auto name : names) {
    function_args_.push_back(FunctionArg(name));
  }
}

FunctionArgTransitionRule::FunctionArgTransitionRule(
    std::vector<FunctionArg> args)
    : function_args_(args), TransitionRule(TransitionTrigger::FUNCTION_ARG) {}

FunctionArgTransitionRule::FunctionArgTransitionRule(std::string name)
    : TransitionRule(TransitionTrigger::FUNCTION_ARG) {
  function_args_.push_back(FunctionArg(name));
}

FunctionArgTransitionRule::FunctionArgTransitionRule(
    const FunctionArgTransitionRule &rule)
    : TransitionRule(rule) {
  function_args_ = rule.function_args_;
}

StoreValueTransitionRule::StoreValueTransitionRule(
    StoreValueType type, std::vector<std::string> funcs)
    : TransitionRule(TransitionTrigger::STORE_VALUE), type_(type),
      function_names_(funcs),
      // Bug fix: default to false so detectors must explicitly opt-in to
      // branch-variant transitions. Previously always true, causing every
      // store rule to silently register NULL_BRANCH_CONSIDERED_* variants.
      consider_null_branch_(false) {}

UseValueTransitionRule::UseValueTransitionRule()
    : TransitionRule(TransitionTrigger::USE_VALUE) {}

AliasValueTransitionRule::AliasValueTransitionRule()
    : TransitionRule(TransitionTrigger::ALIASED_VALUE) {}
} // namespace fitx

// FiTx CFG-based typestate analysis: path-insensitive, inter-procedural,
// return-code aware state propagation (Suzuki et al., USENIX ATC 2024,
// ยง4.2, 4.3).
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

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Instructions/BranchInstruction.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Frontend/StateTransition.h"

// include STL
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Framework_IR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/CommandlineArgs.h"
#include "Checker/FiTx/Frontend/Function.h"
#include "Checker/FiTx/Frontend/PropagationConstraint.h"

#include <algorithm>
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

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace fitx {
Analyzer::Analyzer(llvm::Module &llvm_module,
                   fitx::StateManager &state_manager,
                   fitx::LoggingClient &client)
    : llvm_module_(llvm_module), state_manager_(state_manager), log_(client) {}

// --- Top-level: analyze all functions in this module ---
// framework_ir_ maps each LLVM Module to a list of fitx::Function (our
// CFG + IR). We analyze one compilation unit at a time (paper: single-TU
// scope).
void Analyzer::analyze() {
  auto &framework_ir = ir_generator::IRGenerator::framework_ir_;
  if (framework_ir.find(&llvm_module_) == framework_ir.end())
    return;
  for (auto function : framework_ir[&llvm_module_]) {
    analyzeFunction(function);
  }
  log_.flush();
}

void Analyzer::analyzeFunction(std::shared_ptr<fitx::Function> function) {
  // Bottom-up summary-based analysis (paper ยง4.3): each function analyzed
  // once; states summarized per return code for caller propagation.
  if (!functionInformationExists(function))
    function_info_[function] =
        std::make_shared<fitx::FunctionInformation>(function);

  auto func_info = function_info_[function];
  if (func_info->Stat() != FunctionInformation::UNANALYZED)
    return;
  func_info->setAnalysisStat(
      fitx::FunctionInformation::AnalysisStat::IN_PROGRESS);

  analyzing_function_.push(function);
  // Process blocks in order; createBasicBlockInfo merges states from all
  // predecessors (paper ยง4.2: "collect the state from the predecessor").
  auto target_blocks = function->OrderedBasicBlocks();
  std::queue<std::shared_ptr<fitx::BasicBlock>> block_queue(
      std::deque<std::shared_ptr<fitx::BasicBlock>>(target_blocks.begin(),
                                                         target_blocks.end()));

  while (!block_queue.empty()) {
    auto block = block_queue.front();
    // Per-block state: value -> TransitionLogs (current state + history).
    bb_info_ =
        func_info->createBasicBlockInfo(block, state_manager_.getStates());
    func_info->setAnayzingBasicBlock(block);
    // Apply branch-dependent transitions (e.g. ptr==NULL on true path).
    analyzePrevBlockBranch(block);
    for (auto inst : block->Instructions()) {
      switch (inst->Opcode()) {
      case llvm::Instruction::Call:
        analyzeCallInst(inst);
        break;
      case llvm::Instruction::Store:
        analyzeStoreInst(inst);
        break;
      case llvm::Instruction::Load:
        analyzeLoadInst(inst);
        break;
      default:
        break;
      }
    }
    block_queue.pop();

    // Worklist: if any value state changed (e.g. from callee summary),
    // re-analyze.
    if (func_info->basicBlockInfoChanged(block)) {
      block_queue.push(block);
      continue;
    }

    // Bug-checking hooks (paper Table 6): IMMEDIATE = after transition;
    // END_OF_LIFE = end of variable lifetime.
    generateError(BugNotificationTiming::IMMEDIATE);
    generateError(BugNotificationTiming::END_OF_LIFE, block->DeadValues());
  }

  analyzeReturnValue(function);

  bb_info_ = func_info->getBasicBlockInformation(function->ReturnBlock());
  if (bb_info_) {
    // FUNCTION_END = end of function; MODULE_END = no in-unit callers (paper
    // Table 6).
    generateError(BugNotificationTiming::FUNCTION_END);

    if (function->CallerFunctions().empty())
      generateError(BugNotificationTiming::MODULE_END);
  }

  analyzing_function_.pop();
  func_info->setAnalysisStat(
      fitx::FunctionInformation::AnalysisStat::ANALYZED);
}

// --- Predecessor merge & branch-dependent transitions ---
// For each predecessor that has a branch (e.g. if (ptr == NULL)), we determine
// whether the current block is on the "null" or "non-null" path and apply the
// corresponding store transitions (e.g. "ptr is null" -> transition to init).
// Also used for return-code aware propagation: branch on return value selects
// which callee return codes we propagate (paper ยง4.3).
void Analyzer::analyzePrevBlockBranch(
    std::shared_ptr<fitx::BasicBlock> block) {
  for (auto pred_reference : block->Predecessors()) {
    auto preds = pred_reference.lock();
    if (!preds ||
        !currentFunctionInformation()->getBasicBlockInformation(preds))
      continue;
    /* passthrough_blocks (unused): could refine which pred we use */
    /*     std::vector<std::shared_ptr<fitx::BasicBlock>>(); */

    /* auto weak_blocks = preds->getPassthroughBlock(block); */
    /* std::transform(weak_blocks.begin(), weak_blocks.end(), */
    /*                std::back_inserter(passthrough_blocks), */
    /*                [](const std::weak_ptr<fitx::BasicBlock> block) {
     */
    /*                  return block.lock(); */
    /*                }); */
    /* if (!passthrough_blocks.size()) passthrough_blocks.push_back(preds); */

    /* for (auto passed_block : passthrough_blocks) { */
    auto branch_inst = preds->getBranchInst();
    if (!branch_inst || !branch_inst->Condition())
      continue;

    auto condition_inst =
        shared_dyn_cast<fitx::CompareInst>(branch_inst->Condition());
    if (!condition_inst)
      continue;

    // Identify the compared value and whether the condition involves NULL.
    bool null_exists = false;
    std::shared_ptr<fitx::Value> comp_value = nullptr;
    for (auto operand : condition_inst->Operands()) {
      if (shared_isa<fitx::NullValue>(operand)) {
        null_exists = true;
        continue;
      }
      if (shared_isa<fitx::ConstValue>(operand))
        continue;
      comp_value = operand;
    }
    if (!comp_value)
      continue;

    auto call_inst = shared_dyn_cast<CallInst>(comp_value);
    if (call_inst && call_inst->CalledFunction() &&
        Function::IsExpectFunction(call_inst->CalledFunction())) {
      for (auto args : call_inst->Arguments())
        generateWarning(call_inst.get(), args.get());
    }

    if (!null_exists)
      continue;
    // Which successor set is "null": true branch for (ptr == NULL), false for
    // (ptr != NULL).
    BranchInst::TransitionNodes null_nodes =
        condition_inst->GetPredicate() == llvm::CmpInst::ICMP_EQ
            ? branch_inst->TruePathNodes()
            : branch_inst->FalsePathNodes();
    // If current block is on the null path, use NULL_BRANCH_CONSIDERED_NULL
    // transitions.
    StoreValueTransitionRule::StoreValueType type =
        std::find_if(null_nodes.begin(), null_nodes.end(),
                     [&block](auto node) { return node.lock() == block; }) !=
                null_nodes.end()
            ? StoreValueTransitionRule::NULL_BRANCH_CONSIDERED_NULL
            : StoreValueTransitionRule::NULL_BRANCH_CONSIDERED_NON_NULL;

    // Apply branch-specific transitions plus ANY (for typestates that don't
    // care).
    auto possible_transitions =
        state_manager_.TransitionManager()->getStoreArgTransitions(type);

    auto transitions =
        state_manager_.TransitionManager()->getStoreArgTransitions(
            fitx::StoreValueTransitionRule::NULL_BRANCH_CONSIDERED_ANY);

    transitions.insert(transitions.end(), possible_transitions.begin(),
                       possible_transitions.end());

    // Apply to comp_value and all values that may alias it (intra-procedural
    // alias; paper ยง3).
    std::set<std::shared_ptr<fitx::Value>> related_values =
        currentFunctionInformation()->GetValueCollection().getRelatedValues(
            comp_value);
    related_values.insert(comp_value);
    if (auto aliased = currentFunctionInformation()
                           ->getBasicBlockInformation(preds)
                           ->getAliasValues()
                           .getAliasInfo(comp_value)) {
      related_values.insert(aliased->Values().begin(), aliased->Values().end());
    }

    generateWarning(branch_inst.get(), "Branch Inst Transition");
    for (auto value : related_values) {
      changeValueState(transitions, value, branch_inst);
    }
    generateWarning(branch_inst.get(), "Branch Inst Transition Done");

    // For return-code summary: don't treat this path as returning the other
    // branch's code.
    int remove_ret_val =
        type == fitx::StoreValueTransitionRule::NULL_VAL ? 0 : -1;
    bb_info_->removeReturnvalue(remove_ret_val);
    /* } */
  }
}

// --- Call instruction: apply function-arg transitions, then callee summary ---
// 1) Apply "Fun Arg" transitions (paper Table 5): e.g. kfree(ptr) -> ptr goes
// to free. 2) If indirect call: stop tracking args (paper: no indirect calls).
// 3) If callee is defined in this TU: analyze callee (bottom-up), then
// copyFunctionValues
//    to apply its summary (arg/return states) to the caller.
void Analyzer::analyzeCallInst(std::shared_ptr<fitx::Instruction> I) {
  auto call_inst = std::static_pointer_cast<fitx::CallInst>(I);

  // Apply typestate transitions triggered by "call F with arg i" (e.g. kfree,
  // malloc).
  if (analyzeFunctionCall(call_inst))
    return;

  auto function = call_inst->CalledFunction();
  if (!function) {
    // Indirect call: paper does not consider indirect calls (FiT analysis).
    for (auto value : call_inst->Arguments()) {
      std::set<std::shared_ptr<fitx::Value>> related_values =
          currentFunctionInformation()->GetValueCollection().getRelatedValues(
              value);
      related_values.insert(value);
      for (auto &related : related_values) {
        bb_info_->removeValueFromState(related, I);
      }
    }
    return;
  }

  if (fitx::Function::IsDebugDeclareFunction(function)) {
    bb_info_->resetValueState(call_inst->Arguments()[0], I);
    return;
  }

  if (function->isDebugFunction())
    return;

  // Special Case where memset is called. This is semantically the same as
  // storing something to the target value, so we collect for such info..
  if (Function::IsMemSetFunction(function) && !call_inst->Arguments().empty()) {
    std::vector<fitx::Transition> transitions;
    /* auto target_value = */
    /*     shared_dyn_cast<fitx::ConstValue>(call_inst->Arguments()[1]); */
    /* if (target_value && target_value->getConstValue() == 0) { */
    /*   auto possible_transitions = */
    /*       state_manager_.TransitionManager()->getStoreArgTransitions( */
    /*           fitx::StoreValueTransitionRule::NULL_VAL); */

    /*   transitions.insert(transitions.end(), possible_transitions.begin(), */
    /*                      possible_transitions.end()); */
    /* } */

    auto possible_transitions =
        state_manager_.TransitionManager()->getStoreArgTransitions(
            fitx::StoreValueTransitionRule::ANY);

    transitions.insert(transitions.end(), possible_transitions.begin(),
                       possible_transitions.end());

    std::set<std::shared_ptr<fitx::Value>> related_values =
        currentFunctionInformation()->GetValueCollection().getRelatedValues(
            call_inst->Arguments()[0]);
    related_values.insert(call_inst->Arguments()[0]);

    for (auto value : related_values) {
      changeValueState(transitions, value, I);
    }
    return;
  }

  // External/declaration: we don't have a summary; stop tracking state for args
  // (conservative).
  if (function->isDeclaration()) {
    for (auto value : call_inst->Arguments()) {
      std::set<std::shared_ptr<fitx::Value>> related_values =
          currentFunctionInformation()->GetValueCollection().getRelatedValues(
              value);
      related_values.insert(value);
      for (auto &related : related_values) {
        bb_info_->removeValueFromState(related, I);
      }
    }
  } else {
    // Callee defined in this TU: analyze it (bottom-up), then apply its
    // summary.
    if (state_manager_.getStatefulConstraint() &&
        !state_manager_.getStatefulConstraint()->shouldPropagateOnCallInst(
            call_inst)) {
      return;
    }
    analyzeFunction(function);
    bb_info_ = function_info_[analyzing_function_.top()]
                   ->getCurrentBasicBlockInformation();
    copyFunctionValues(function, call_inst); // Apply callee summary to caller.
  }
}

// --- Store: apply store transitions to the pointer operand (and aliases) ---
// Transitions depend on what is stored: NULL (Store NULL), non-null (Store
// NON), result of a specific call (Store Const / CALL_FUNC, e.g. malloc), or
// any (Store ANY). Paper Table 5: Store NULL/NON/Const/ANY trigger typestate
// updates on the stored-to location.
void Analyzer::analyzeStoreInst(std::shared_ptr<fitx::Instruction> I) {
  auto store_inst = std::static_pointer_cast<fitx::StoreInst>(I);
  std::vector<fitx::Transition> transitions;

  auto value_operand = store_inst->ValueOperand();
  if (fitx::shared_isa<fitx::NullValue>(value_operand)) {
    auto possible_transitions =
        state_manager_.TransitionManager()->getStoreArgTransitions(
            fitx::StoreValueTransitionRule::NULL_VAL);
    transitions.insert(transitions.end(), possible_transitions.begin(),
                       possible_transitions.end());
  } else {
    auto possible_transitions =
        state_manager_.TransitionManager()->getStoreArgTransitions(
            fitx::StoreValueTransitionRule::NON_NULL_VAL);
    transitions.insert(transitions.end(), possible_transitions.begin(),
                       possible_transitions.end());
  }

  // If storing result of a known function (e.g. kmalloc), add CALL_FUNC
  // transitions.
  if (auto call_inst =
          fitx::shared_dyn_cast<fitx::CallInst>(value_operand)) {
    auto called_func = call_inst->CalledFunction();
    if (called_func) {
      auto possible_transitions =
          state_manager_.TransitionManager()->getStoreArgTransitions(
              fitx::StoreValueTransitionRule::CALL_FUNC,
              called_func->Name());
      transitions.insert(transitions.end(), possible_transitions.begin(),
                         possible_transitions.end());
    }
  }

  auto possible_transitions =
      state_manager_.TransitionManager()->getStoreArgTransitions(
          fitx::StoreValueTransitionRule::ANY);
  transitions.insert(transitions.end(), possible_transitions.begin(),
                     possible_transitions.end());

  // Apply to the stored-to pointer and related (e.g. same base) values.
  std::set<std::shared_ptr<fitx::Value>> related_values =
      currentFunctionInformation()->GetValueCollection().getRelatedValues(
          store_inst->PointerOperand());
  related_values.insert(store_inst->PointerOperand());
  for (auto value : related_values) {
    changeValueState(transitions, value, I);
  }

  checkAlias(
      store_inst); // Record ptr = value_operand and apply alias transitions.
}

// Record may-alias (pointer_operand = value_operand) in current block's
// AliasValues, then apply alias transitions to pointer + related + aliased
// values. Alias is intra-procedural and built during analysis (paper ยง3).
void Analyzer::checkAlias(std::shared_ptr<fitx::StoreInst> store_inst) {
  auto value_operand = store_inst->ValueOperand();

  const ValueCollection *aliased = currentFunctionInformation()
                                       ->getCurrentBasicBlockInformation()
                                       ->getAliasValues()
                                       .getAliasInfo(value_operand);

  currentFunctionInformation()
      ->getCurrentBasicBlockInformation()
      ->getAliasValues()
      .addAlias(store_inst->PointerOperand(), store_inst->ValueOperand());

  if (shared_isa<fitx::ConstValue>(value_operand) ||
      shared_isa<fitx::NullValue>(value_operand) ||
      (shared_isa<fitx::CallInst>(value_operand) && !aliased))
    return;

  std::set<std::shared_ptr<fitx::Value>> related_values =
      currentFunctionInformation()->GetValueCollection().getRelatedValues(
          store_inst->PointerOperand());
  related_values.insert(store_inst->PointerOperand());

  if (!shared_isa<fitx::CallInst>(value_operand)) {
    auto value_related =
        currentFunctionInformation()->GetValueCollection().getRelatedValues(
            value_operand);

    related_values.insert(value_related.begin(), value_related.end());
    related_values.insert(value_operand);
  }

  if (aliased) {
    auto values = aliased->Values();
    related_values.insert(values.begin(), values.end());
  }

  auto possible_transitions =
      state_manager_.TransitionManager()->getAliasTransitions();
  for (auto value : related_values) {
    changeValueState(possible_transitions, value, store_inst);
  }
}

// --- Load: apply "Use" transitions (paper Table 5: Use load loadee) ---
// e.g. for UAF typestate, using a freed pointer triggers the bug state.
void Analyzer::analyzeLoadInst(std::shared_ptr<fitx::Instruction> I) {
  auto load_inst = std::static_pointer_cast<fitx::LoadInst>(I);
  auto transitions =
      state_manager_.TransitionManager()->getUseValueTransitions();
  changeValueState(transitions, load_inst->LoadValue(), I);
}

bool Analyzer::functionInformationExists(
    std::shared_ptr<fitx::Function> function) {
  return function_info_.find(function) != function_info_.end();
}

// Apply a list of transitions to a value in the current block's state.
// changeValueState in BasicBlockInformation tries each transition (source state
// must match); if one applies, the value's state is updated and we record for
// worklist.
void Analyzer::changeValueState(std::vector<Transition> transitions,
                                std::shared_ptr<Value> value,
                                std::shared_ptr<fitx::Instruction> inst) {
  if (value->isGlobalVar() || transitions.empty())
    return;
  if (currentFunctionInformation()
          ->getCurrentBasicBlockInformation()
          ->changeValueState(transitions, value, inst)) {
    currentFunctionInformation()->addValue(value);
    fitx::generateWarning(inst.get(),
                               "[Value Change] Change Value State: ");
    fitx::generateWarning(inst.get(), value.get());
  }
}

// Emit a bug report for every value that is in a bug state at this hook.
// We only report if LeastSignificantSource is init: reduces false positives by
// requiring the value to have reached the bug state from a "real" init (paper
// ยง4.2: priority/distance to bug state used when merging predecessors).
void Analyzer::generateError(
    BugNotificationTiming timing,
    const std::set<std::shared_ptr<fitx::Value>> values) {
  for (auto state : state_manager_.getBugStates()) {
    if (state.NotificationTiming() != timing)
      continue;
    for (auto value : bb_info_->getValueTransitionStates(state)) {
      if (!values.empty() && values.find(value.first) == values.end())
        continue;
      if (value.second->CurrentState() != state)
        continue;
      if (!value.second->LeastSignificantSource().isInitState())
        continue;
      /* if (!value.second->ReducedTransition().Source().isInitState())
       * continue; */

      if (state.getTriggerConstraint() == TriggerConstraint::NON_RETURN &&
          value.first ==
              currentFunctionInformation()->Function()->getReturnValue()) {
        continue;
      }

      if (fitx::CommandLineArgs::Flex ||
          !value.first->isArbitaryArrayElement()) {
        llvm::raw_string_ostream log_stream(log_.getBuffer());
        fitx::generateError(log_stream,
                                 value.second->CurrentInstruction().get(),
                                 "--- [" + state.Name() + "] ---");
        fitx::generateError(log_stream,
                                 value.second->CurrentInstruction().get(),
                                 value.first.get());
        value.second->generateLog(log_stream);
      }
      value.second->logicalTerminate(value.second->CurrentInstruction());
    }
  }
}

// Apply callee summary to caller: map callee arg states to caller's call-site
// operands. If the return value is used in a branch, addPendingFunctionValues
// already set pending states per (successor block, return code); we only reach
// here when we don't have that (e.g. return value not checked). Then we merge
// states from all "success" blocks of the callee and apply to caller (paper
// ยง4.3).
void Analyzer::copyFunctionValues(
    std::shared_ptr<fitx::Function> called_func,
    std::shared_ptr<fitx::CallInst> call_inst) {
  if (!functionInformationExists(called_func))
    return;
  auto called_func_info = function_info_[called_func];

  if (called_func->ProtectedRefcountValue() &&
      called_func->lastRefcountInstruction() != call_inst) {
    return; // Refcount special-case: skip if not the last refcount call.
  }

  // If caller branches on return value, we already set pending states per
  // branch.
  if (addPendingFunctionValues(called_func, call_inst))
    return;

  // No return-code branch: merge states from all callee exit blocks and apply.
  auto success_blocks = called_func_info->getSuccessBlock();
  if (success_blocks.empty())
    return;

  auto operands = call_inst->Arguments();
  ArgValueStates pending_states(operands.size(), state_manager_.getStates());
  for (auto success_block_ref : success_blocks) {
    auto success_block = success_block_ref.lock();
    if (!success_block)
      continue;
    auto basic_block_info =
        called_func_info->getBasicBlockInformation(success_block);

    if (!basic_block_info)
      continue;
    pending_states.addArgValueState(basic_block_info->getArgValueStates());

    /* llvm::errs() << "=== :(\n"; */
    /* auto operands = call_inst->Arguments(); */
    /* for (int i = 0; i < operands.size(); i++) { */
    /*   auto operand = operands[i]; */
    /*   for (auto value : */
    /*        basic_block_info->getArgValueStates().getValueStateForArg(i)) { */
    /*     llvm::errs() << "=== " << i << "\n"; */
    /*     llvm::errs() << value.first; */
    /*     for (auto trans : value.second) { */
    /*       llvm::errs() << trans << " (Transitions)\n"; */
    /*     } */
    /* auto new_value = Value::CreateAppend(operand, value.first); */
    /* changeValueState(value.second, new_value, call_inst); */
    /* } */
    /* } */
    /* llvm::errs() << "=== :)\n"; */
  }

  // Map callee arg i (value.first) + its transitions (value.second) to caller
  // operand: new_value = operand (caller) extended with value.first (callee
  // arg).
  generateWarning(call_inst.get(), "Call Inst Here");
  for (int i = 0; i < operands.size(); i++) {
    auto operand = operands[i];
    for (auto value : pending_states.getValueStateForArg(i)) {
      auto new_value = Value::CreateAppend(operand, value.first);
      if (called_func->ProtectedRefcountValue() &&
          fitx::shared_isa<fitx::Argument>(new_value))
        continue;
      changeValueState(value.second, new_value, call_inst);
    }
  }
}

// Return-code aware state propagation (paper ยง4.3): if the return value is
// used in a branch (e.g. if (err)), propagate only the states for the return
// codes that satisfy that branch (e.g. error path gets error-return states).
bool Analyzer::addPendingFunctionValues(
    std::shared_ptr<fitx::Function> called_func,
    std::shared_ptr<fitx::CallInst> call_inst) {
  auto branch_inst =
      currentFunctionInformation()->currentBasicBlock()->getBranchInst();
  if (!branch_inst || !branch_inst->Condition())
    return false;

  generateWarning(call_inst.get(), "Found BranchInst");

  int compared_value = fitx::FunctionInformation::kSuccessCode;
  llvm::CmpInst::Predicate predicate = llvm::CmpInst::Predicate::ICMP_NE;
  bool is_null_value = false;

  if (auto compare_inst = fitx::shared_dyn_cast<fitx::CompareInst>(
          branch_inst->Condition())) {
    if (!compare_inst->operandExists(call_inst)) {
      return false;
    }
    generateWarning(call_inst.get(), "Found CompareInst");

    predicate = compare_inst->GetPredicate();

    auto operand = std::find_if(
        compare_inst->Operands().begin(), compare_inst->Operands().end(),
        [call_inst](auto operand) { return operand != call_inst; });

    if (operand == compare_inst->Operands().end())
      return false;

    if (auto const_value =
            fitx::shared_dyn_cast<fitx::ConstValue>(*operand)) {
      compared_value = const_value->getConstValue();
    } else if (fitx::shared_isa<fitx::NullValue>(*operand)) {
      is_null_value = true;
    }
  } else if (fitx::shared_isa<fitx::CallInst>(
                 branch_inst->Condition())) {
    if (call_inst != branch_inst->Condition())
      return false;
    generateWarning(call_inst.get(), "Found Call Inst instead");
  }

  if (!functionInformationExists(called_func))
    return false;
  auto called_func_info = function_info_[called_func];
  generateWarning(call_inst.get(), "Found called func info");

  // For each (return_code, blocks) in callee summary, decide if this branch
  // takes the "false" path for that code (e.g. if (err) and ret.first == 0 ->
  // false path).
  auto basic_block_info =
      currentFunctionInformation()->getCurrentBasicBlockInformation();
  for (auto ret : called_func_info->getReturnValueInfo()) {
    bool is_false_path = false;
    switch (predicate) {
    case llvm::CmpInst::Predicate::ICMP_EQ:
      is_false_path = ret.first == compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_NE:
      is_false_path = ret.first != compared_value;
      if (is_null_value)
        is_false_path = !is_false_path;
      break;
    case llvm::CmpInst::Predicate::ICMP_SLE:
    case llvm::CmpInst::Predicate::ICMP_ULE:
      is_false_path = ret.first >= compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_SGE:
    case llvm::CmpInst::Predicate::ICMP_UGE:
      is_false_path = ret.first <= compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_SLT:
    case llvm::CmpInst::Predicate::ICMP_ULT:
      is_false_path = ret.first < compared_value;
      break;
    case llvm::CmpInst::Predicate::ICMP_SGT:
    case llvm::CmpInst::Predicate::ICMP_UGT:
      is_false_path = ret.first > compared_value;
      break;
    default:
      break;
      // TODO
    }
    generateWarning(call_inst.get(),
                    std::to_string(ret.first) + " Called Func");
    generateWarning(call_inst.get(),
                    is_false_path ? "True Path" : "False Path");

    // For each caller successor block on the chosen path, record pending arg
    // states from the callee blocks that return this return code.
    BranchInst::TransitionNodes nodes = is_false_path
                                            ? branch_inst->TruePathNodes()
                                            : branch_inst->FalsePathNodes();
    for (auto successor_node : nodes) {
      auto ret_value = std::make_shared<fitx::ConstValue>(ret.first);
      auto locked = successor_node.lock();
      for (auto success_block_ref : ret.second) {
        auto success_block = success_block_ref.lock();
        if (!success_block)
          continue;
        auto success_basic_block_info =
            called_func_info->getBasicBlockInformation(success_block);

        if (!success_block->Instructions().empty())
          generateWarning(success_block->Instructions().front().get(),
                          "Pred Blocks");

        if (!locked->Instructions().empty())
          generateWarning(locked->Instructions().front().get(),
                          "Propagating Block");

        if (!success_basic_block_info)
          continue;

        basic_block_info->setPendingValueStates(
            successor_node, success_basic_block_info->getArgValueStates());
        basic_block_info->setPendingReturnValues(successor_node, ret_value);
      }
    }
  }

  currentFunctionInformation()->addValues(
      called_func_info->GetValueCollection());
  return true;
}

// Apply "Fun Arg" transition rules (paper Table 5: Fun Arg call arg arg num).
// For each argument index, look up transitions (e.g. kfree(ptr) -> ptr:
// initโ��free) and apply to the argument value (and optionally parent/related
// values).
bool Analyzer::analyzeFunctionCall(
    std::shared_ptr<fitx::CallInst> call_inst) {
  auto function = call_inst->CalledFunction();
  if (!function || call_inst->Arguments().empty())
    return false;

  bool changed = false;
  for (int arg = 0; arg < call_inst->Arguments().size(); arg++) {
    const std::pair<FunctionArgTransitionRule::FunctionArg,
                    std::vector<fitx::Transition>>
        transitions =
            state_manager_.TransitionManager()->getFunctionArgTransitions(
                function->Name(), arg);
    changed = changed || !transitions.second.empty();

    std::set<std::shared_ptr<fitx::Value>> args;
    if (transitions.first.consider_parent) {
      args = currentFunctionInformation()->GetValueCollection().getParentValues(
          call_inst->Arguments()[arg]);
    }
    args.insert(call_inst->Arguments()[arg]);

    for (auto &value : args) {
      changeValueState(transitions.second, value, call_inst);
    }
  }
  return changed;
}

// Collect possible return codes per predecessor block for function summary
// (paper ยง4.3). Constant return values (e.g. 0, -ENOMEM) are used to key
// summarized states so callers can propagate the right state per branch.
void Analyzer::analyzeReturnValue(
    std::shared_ptr<fitx::Function> function) {
  auto return_block = function->ReturnBlock();
  auto func_info = getFunctionInformation(function);
  if (!return_block || !func_info)
    return;

  if (!function->ReturnType()->isIntOrPtrTy()) {
    func_info->addReturnValueInfo(FunctionInformation::kSuccessCode,
                                  return_block);
    return;
  }

  std::set<std::weak_ptr<fitx::BasicBlock>> pred_block(
      return_block->Predecessors());

  // Bug fix: the original logic cleared pred_block when the return block had
  // instructions, then unconditionally re-inserted return_block whenever
  // pred_block was empty — making the two branches non-exclusive and silently
  // discarding the predecessor set for functions whose return block has no
  // predecessors. Correct intent: if the return block itself has instructions
  // (i.e. it is not a pure merge block), analyze it directly; otherwise use
  // its predecessors.
  if (!return_block->Instructions().empty() || pred_block.empty()) {
    pred_block.clear();
    pred_block.insert(return_block);
  }

  // Build function summary: for each predecessor of the return block, collect
  // which constant return values it may produce; record (return_code, pred) in
  // func_info so callers can propagate by return code (paper ยง4.3).
  for (auto pred_reference : pred_block) {
    if (auto pred = pred_reference.lock()) {
      if (!pred->Instructions().empty()) {
        generateWarning(pred->Instructions().front().get(),
                        "Pred Block analysis");
      }

      auto block_info = func_info->getBasicBlockInformation(pred);
      if (!block_info)
        continue;
      if (block_info->ReturnValues().empty()) {
        func_info->addReturnValueInfo(FunctionInformation::kSuccessCode, pred);
        func_info->addReturnValueInfo(FunctionInformation::kErrorCode, pred);
        continue;
      }

      for (auto return_value : block_info->ReturnValues()) {
        if (auto const_int = fitx::shared_dyn_cast<fitx::ConstValue>(
                return_value)) {
          auto value = const_int->getConstValue();
          generateWarning(std::to_string(value));
          if (block_info->ReturnValueSatisfiable(value))
            func_info->addReturnValueInfo(value, pred);
        } else if (auto const_null =
                       fitx::shared_dyn_cast<fitx::NullValue>(
                           return_value)) {
          func_info->addReturnValueInfo(FunctionInformation::kErrorCode, pred);
          // Add the value to the list
        } else if (auto call_inst =
                       fitx::shared_dyn_cast<fitx::CallInst>(
                           return_value)) {
          auto called_function = call_inst->CalledFunction();
          if (called_function) {
            if (called_function->isErrorFunction()) {
              func_info->addReturnValueInfo(FunctionInformation::kErrorCode,
                                            pred);
              continue;
            }

            auto called_func_info = getFunctionInformation(called_function);
            if (!called_func_info) {
              /* func_info->addReturnValueInfo(FunctionInformation::kErrorCode,
               */
              /*                               pred); */
              /* func_info->addReturnValueInfo(FunctionInformation::kSuccessCode,
               */
              /*                               pred); */
              continue;
            }

            for (auto return_value : called_func_info->getReturnValueInfo()) {
              /* func_info->addReturnValueInfo(return_value.first, */
              /*                               return_value.second); */
              if (block_info->ReturnValueSatisfiable(return_value.first))
                func_info->addReturnValueInfo(return_value.first, pred);
            }
          }
          // Decode the call_inst by chaning
        } else {
          if (return_value->getValueID() > 0) {
            func_info->addReturnValueInfo(FunctionInformation::kSuccessCode,
                                          pred);
            func_info->addReturnValueInfo(FunctionInformation::kErrorCode,
                                          pred);
          }
          // is success block
        }
      }
    }
  }
}

std::shared_ptr<FunctionInformation> Analyzer::getFunctionInformation(
    std::shared_ptr<fitx::Function> function) {
  auto func_info = function_info_.find(function);
  if (func_info != function_info_.end())
    return func_info->second;
  return nullptr;
}

} // namespace fitx

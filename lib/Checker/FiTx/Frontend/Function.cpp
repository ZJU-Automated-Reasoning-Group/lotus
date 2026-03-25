#include "llvm/IR/Function.h"

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

#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Value.h"

// include STL
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/BasicBlock.h"
#include "Checker/FiTx/Frontend/Function.h"

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
FunctionInformation::FunctionInformation(
    std::shared_ptr<fitx::Function> function, AnalysisStat stat)
    : framework_function_(function), stat_(stat) {}

void FunctionInformation::setAnalysisStat(
    FunctionInformation::AnalysisStat stat) {
  stat_ = stat;
}

void FunctionInformation::setAnayzingBasicBlock(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  current_basicblock_ = basic_block;
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::getCurrentBasicBlockInformation() {
  return basic_block_info_[current_basicblock_];
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::createBasicBlockInfo(
    std::shared_ptr<fitx::BasicBlock> basic_block,
    const std::set<State> &states) {
  int time_to_live = BasicBlockInformation::kMaxTimeToLive;

  if (basicBlockInfoExists(basic_block)) {
    prev_basic_block_info_[basic_block] = basic_block_info_[basic_block];
    time_to_live = prev_basic_block_info_[basic_block]->TimeToLive() - 1;
  }

  auto current_block_info = basic_block_info_[basic_block] =
      std::make_shared<BasicBlockInformation>(basic_block, states);

  current_block_info->setTimeToLive(time_to_live);

  if (basic_block->isCleanupBlock())
    return current_block_info;

  bool return_value_assigned = !current_block_info->ReturnValues().empty();
  // Merge typestate and arg summaries from all predecessors (paper §4.2).
  for (auto pred_reference : basic_block->Predecessors()) {
    if (auto preds = pred_reference.lock()) {
      if (!basicBlockInfoExists(preds)) {
        current_block_info->setPartialStates(true);
        continue;
      }
      auto passthrough_blocks =
          std::vector<std::shared_ptr<fitx::BasicBlock>>();

      auto weak_blocks = preds->getPassthroughBlock(basic_block);
      std::transform(weak_blocks.begin(), weak_blocks.end(),
                     std::back_inserter(passthrough_blocks),
                     [](const std::weak_ptr<fitx::BasicBlock> block) {
                       return block.lock();
                     });
      if (!passthrough_blocks.size())
        passthrough_blocks.push_back(preds);

      for (auto block : passthrough_blocks) {
        if (!basicBlockInfoExists(block)) {
          current_block_info->setPartialStates(true);
          continue;
        }

        auto pred_block_info = basic_block_info_[block];
        if (pred_block_info->PartialStates()) {
          if (block->Line() < basic_block->Line())
            current_block_info->setPartialStates(true);
          else if (pred_block_info->TimeToLive() > 0)
            current_block_info->setPartialStates(true);
          continue;
        }

        // TODO: Fix this rough check of error code propagation
        BasicBlockInformation::BlockStatus status =
            pred_block_info->getBlockStatus();
        const auto &branch_inst =
            pred_block_info->BasicBlock()->getBranchInst();
        if (branch_inst && branch_inst->Condition() &&
            branch_inst->returnValueOperandExists()) {
          status = BasicBlockInformation::ERROR;
          if (auto condition =
                  shared_dyn_cast<CompareInst>(branch_inst->Condition())) {
            // Bug fix: add missing break statements to prevent fall-through.
            // Previously ICMP_EQ and ICMP_SGT fell through to set ERROR,
            // making the SUCCESS assignment unreachable.
            switch (condition->GetPredicate()) {
            case llvm::CmpInst::Predicate::ICMP_EQ:
            case llvm::CmpInst::Predicate::ICMP_SGT:
              status = BasicBlockInformation::SUCCESS;
              break;
            case llvm::CmpInst::Predicate::ICMP_NE:
            case llvm::CmpInst::Predicate::ICMP_SLT:
              status = BasicBlockInformation::ERROR;
              break;
            default:
              break;
            }
          }
        }
        current_block_info->setBlockStatus(status);

        auto pred_value_states =
            pred_block_info->ValueStatesForSuccessor(basic_block);
        for (auto val_states : pred_value_states.first.ValueStates()) {
          auto &value = val_states.first;
          if (!current_block_info->ValueStates().valueExists(value)) {
            current_block_info->ValueStates().setValueState(
                value, pred_value_states.first.getTransitionLog(value));
            continue;
          }

          auto pred_state = pred_value_states.first.getState(value);
          auto curr_state = current_block_info->ValueStates().getState(value);
          if (pred_state < curr_state) {
            current_block_info->ValueStates().setValueState(
                value, pred_value_states.first.getTransitionLog(value));
          }

          /* if (curr_state < pred_state) { */
          /*   current_block_info->ValueStates().setValueState( */
          /*       value, pred_value_states.first.getTransitionLog(value)); */
          /* } */
        }

        /* if (!basic_block->Instructions().empty()) */
        /*   generateWarning(basic_block->Instructions().front().get(), */
        /*                   "Target"); */
        if (!return_value_assigned) {
          current_block_info->addReturnValues(
              pred_block_info->ReturnCodeForSuccessor(basic_block));
        }

        current_block_info->getArgValueStates().addArgValueState(
            pred_value_states.second);

        /* generateWarning(pred_block_info->BasicBlock().get() ,"---"); */
        /* for (auto ret_val : current_block_info->ReturnValues()) { */
        /*   if (auto cv = fitx::shared_dyn_cast<ConstValue>(ret_val)) { */
        /*     generateWarning(current_block_info->BasicBlock().get(),
         * std::to_string(cv->getConstValue())); */
        /*   } */
        /* } */
        /* generateWarning("---"); */

        // Alias info is not merged across predecessors (intra-block only).
        // TODO: Experimental: Enable when in use
        /* current_block_info->getAliasValues().addAlias( */
        /*     pred_block_info->getAliasValues()); */
      }
    }
  }

  if (current_block_info->getBlockStatus() == BasicBlockInformation::NONE)
    current_block_info->setBlockStatus(BasicBlockInformation::NUTRAL);

  return current_block_info;
}

void FunctionInformation::addValue(std::shared_ptr<Value> value) {
  value_collection_.add(value);
}

void FunctionInformation::addValues(const ValueCollection &collection) {
  value_collection_.add(collection);
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::getBasicBlockInformation(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  if (basicBlockInfoExists(basic_block))
    return basic_block_info_[basic_block];
  return nullptr;
}

std::shared_ptr<BasicBlockInformation>
FunctionInformation::getBasicBlockPrevInformation(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  if (basicBlockPrevInfoExists(basic_block))
    return prev_basic_block_info_[basic_block];
  return nullptr;
}

bool FunctionInformation::basicBlockInfoExists(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  return basic_block_info_.find(basic_block) != basic_block_info_.end();
}

bool FunctionInformation::basicBlockPrevInfoExists(
    std::shared_ptr<fitx::BasicBlock> basic_block) {
  return prev_basic_block_info_.find(basic_block) !=
         prev_basic_block_info_.end();
}

const FunctionInformation::WeakBasicBlockSet &
FunctionInformation::getErrorBlocks(int64_t error_code) {
  return return_info_[error_code];
}

const FunctionInformation::WeakBasicBlockSet &
FunctionInformation::getSuccessBlock() {
  return return_info_[kSuccessCode];
}

bool FunctionInformation::basicBlockInfoChanged(
    std::shared_ptr<fitx::BasicBlock> block) {
  auto current_info = getBasicBlockInformation(block);
  auto prev_info = getBasicBlockPrevInformation(block);
  if (!current_info || !prev_info)
    return true;

  if (current_info->PartialStates())
    return true;

  if (current_info->TimeToLive() <= 0)
    return false;

  /* if (*current_info == *prev_info || !current_info->TimeToLive()) */
  if (*current_info == *prev_info)
    return false;
  return true;
}

void FunctionInformation::addReturnValueInfo(
    int64_t value, std::weak_ptr<fitx::BasicBlock> block) {
  /* auto block_info = getBasicBlockInformation(block.lock()); */
  /* if (!block_info) return; */
  /* if (block_info->ReturnValueSatisfiable(value)) */
  return_info_[value].insert(block);
}

void FunctionInformation::addReturnValueInfo(int64_t value,
                                             WeakBasicBlockSet block_info) {
  if (return_info_.find(value) == return_info_.end()) {
    return_info_[value] = block_info;
    return;
  }
  return_info_[value].insert(block_info.begin(), block_info.end());
}

bool FunctionInformation::existsInRefcountFunctions(
    std::shared_ptr<fitx::Function> function) {
  return std::find(called_refcount_functions_.begin(),
                   called_refcount_functions_.end(),
                   function) != called_refcount_functions_.end();
}

void FunctionInformation::addRefcountFunction(
    std::shared_ptr<fitx::Function> function) {
  called_refcount_functions_.push_back(function);
}

}; // namespace fitx

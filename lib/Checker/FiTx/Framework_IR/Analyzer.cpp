// FiTx framework IR builder: converts llvm::Function to fitx::Function
// (basic blocks, ordered block list, call/store/load/ret instructions, return
// assignments). No typestate; that is done by Frontend::Analyzer (paper
// §4.2, 4.3).
#include "Checker/FiTx/Framework_IR/Analyzer.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Framework_IR/Utils.h"

namespace ir_generator {
Analyzer::Analyzer() {}

// Build fitx::Function: create blocks, iterate instructions to build
// CallInst/StoreInst/LoadInst and collect return assignments; set ordered
// block list for Frontend CFG traversal (paper §4.2).
void Analyzer::analyze(llvm::Function &function, llvm::LoopInfo &loop_info) {
  framework_function_ = fitx::Function::createManagedFunction(
      &function, std::make_unique<llvm::LoopInfo>(std::move(loop_info)));

  std::vector<std::shared_ptr<fitx::BasicBlock>> block;
  for (auto &basic_block : function) {
    auto framework_block = framework_function_->getBasicBlock(&basic_block);
    if (framework_block->Id() == fitx::BasicBlock::kNoId) {
      framework_block->setId(block.size());
      block.push_back(framework_block);
    }
    for (auto &instruction : basic_block) {
      switch (instruction.getOpcode()) {
      case llvm::Instruction::Call:
        analyzeCallInst(&instruction);
        break;
      case llvm::Instruction::Store:
        analyzeStoreInst(&instruction);
        break;
      case llvm::Instruction::Load:
        analyzeLoadInst(&instruction);
        break;
      case llvm::Instruction::Ret:
        analyzeReturnInst(&instruction);
        break;
      default:
        break;
      }
    }
  }
  framework_function_->addOrderedBlock(block);
}

// Build framework representation of a call: create CallInst, attach to block.
// Debug/lifetime/refcount helpers are handled (dead value, protected refcount).
void Analyzer::analyzeCallInst(llvm::Instruction *instruction) {
  auto *call_inst = llvm::cast<llvm::CallInst>(instruction);
  auto framework_block =
      framework_function_->getBasicBlock(instruction->getParent());
  auto framework_call = fitx::CallInst::Create(call_inst);

  if (framework_call->CalledFunction()) {
    if (fitx::Function::IsDebugValueFunction(
            framework_call->CalledFunction())) {
      auto return_block = framework_function_->ReturnBlock();
      const auto &args = framework_call->Arguments();
      if (return_block && !args.empty() && args[0]) {
        return_block->addDeadValue(args[0]);
      }
    }

    if ((framework_call->CalledFunction()->isDebugFunction() &&
         !fitx::Function::IsDebugDeclareFunction(
             framework_call->CalledFunction())) ||
        fitx::Function::IsLifetimeEndFunction(
            framework_call->CalledFunction()))
      return;

    if (fitx::Function::IsRefcountDecrementFunction(
            framework_call->CalledFunction())) {
      framework_function_->setProtectedRefcountValue(
          framework_call->Arguments()[0]);
    }

    if (framework_call->CalledFunction()->ProtectedRefcountValue()) {
      framework_function_->addRefcountInstruction(framework_call);
    }
  }

  framework_block->addInstruction(framework_call);
}

void Analyzer::analyzeDebugCallInst(llvm::CallInst *call_inst) {}

void Analyzer::analyzeStoreInst(llvm::Instruction *inst) {
  auto *store_inst = llvm::cast<llvm::StoreInst>(inst);
  auto framework_block = framework_function_->getBasicBlock(inst->getParent());

  // Paper §3.1.1: omit simple arg→alloca copy from state flow (FiT analysis).
  if (llvm::isa<llvm::Argument>(store_inst->getValueOperand()) &&
      llvm::isa<llvm::AllocaInst>(store_inst->getPointerOperand()))
    return;

  auto created_store = fitx::StoreInst::Create(store_inst);
  framework_block->addInstruction(created_store);
  if (auto call_inst = fitx::shared_dyn_cast<fitx::CallInst>(
          created_store->ValueOperand())) {
    auto branch_inst = framework_block->getBranchInst();
    if (!branch_inst || !branch_inst->Condition())
      return;

    if (!call_inst->CalledFunction() ||
        !call_inst->CalledFunction()->ReturnType()->isIntegerTy())
      return;

    if (auto compare_inst = fitx::shared_dyn_cast<fitx::CompareInst>(
            branch_inst->Condition())) {
      compare_inst->replaceOperand(created_store->PointerOperand(),
                                   created_store->ValueOperand());
    }
  }
}

void Analyzer::analyzeLoadInst(llvm::Instruction *inst) {
  auto *load_inst = llvm::cast<llvm::LoadInst>(inst);

  auto framework_block = framework_function_->getBasicBlock(inst->getParent());
  auto framework_load = fitx::LoadInst::Create(load_inst);

  // TODO: this is a simple check of the load inst.
  std::deque<llvm::User *> users(inst->user_begin(), inst->user_end());
  while (!users.empty()) {
    auto *user = users.front();
    if (llvm::isa<llvm::CallInst>(user) || llvm::isa<llvm::CmpInst>(user) ||
        llvm::isa<llvm::ReturnInst>(user))
      return;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
      if (store->getPointerOperand() == inst)
        return;
    }

    if (auto *unary = llvm::dyn_cast<llvm::UnaryInstruction>(user))
      users.insert(users.end(), unary->user_begin(), unary->user_end());

    users.pop_front();
  }

  framework_block->addInstruction(framework_load);
}

// Record return block and return value; collect possible constant return values
// (e.g. 0, -ENOMEM) by following use-def for building function summary (paper
// §4.3).
void Analyzer::analyzeReturnInst(llvm::Instruction *instruction) {
  auto *return_inst = llvm::cast<llvm::ReturnInst>(instruction);
  auto *return_value = return_inst->getReturnValue();
  framework_function_->setReturnBlock(
      framework_function_->getBasicBlock(return_inst->getParent()));

  if (!return_value || !framework_function_->ReturnType()->isIntOrPtrTy())
    return;

  auto framework_value = fitx::Value::CreateFromDefinition(return_value);
  framework_function_->setReturnValue(framework_value);

  std::vector<llvm::Instruction *> visited_inst{return_inst};
  collectPossibleReturnValues(return_value, visited_inst);
}

// Collect possible return values per block for function summary (paper §4.3).
// Constant return values (e.g. 0, -ENOMEM) key summarized states for
// return-code aware propagation at call sites.
bool Analyzer::collectPossibleReturnValues(
    llvm::Value *V, std::vector<llvm::Instruction *> &visited_inst) {
  auto framework_value = fitx::Value::CreateFromDefinition(V);
  bool add_return_value = false;

  if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(V)) {
    if (std::find(visited_inst.begin(), visited_inst.end(), instruction) !=
        visited_inst.end())
      return true;
    /* return llvm::isa<llvm::CallInst>(instruction) || */
    /*        llvm::isa<llvm::StoreInst>(instruction); */

    visited_inst.push_back(instruction);
  }

  framework_value->setReturnValue(true);
  // Constant return (e.g. 0, -ENOMEM): record (value, block) for summary.
  if (llvm::isa<llvm::Constant>(V)) {
    if (!visited_inst.empty()) {
      auto current_block =
          framework_function_->getBasicBlock(visited_inst.back()->getParent());
      framework_function_->addPossibleReturnValues(framework_value,
                                                   current_block);
      add_return_value = true;
    }
    return add_return_value;
  }

  if (auto *call_inst = llvm::dyn_cast<llvm::CallInst>(V)) {
    auto current_block =
        framework_function_->getBasicBlock(call_inst->getParent());
    framework_function_->addPossibleReturnValues(framework_value,
                                                 current_block);
    add_return_value = true;
  } else if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(V)) {
    add_return_value =
        collectPossibleReturnValues(LI->getPointerOperand(), visited_inst);
    /* for (auto usr : LI->getPointerOperand()->users()) { */
    /*   llvm::Value* val = usr; */
    /*   add_return_value = */
    /*       collectPossibleReturnValues(usr, visited_inst) || add_return_value;
     */
    /* } */
  } else if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(V)) {
    // Bug fix: PHI nodes were completely ignored, causing functions that return
    // via a PHI (the common case after if/else with different return codes) to
    // have no return codes recorded in their summary, breaking
    // return-code-aware propagation. Recurse into each incoming value of the
    // PHI.
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); i++) {
      llvm::Value *incoming = PHI->getIncomingValue(i);
      if (incoming != V)
        add_return_value =
            collectPossibleReturnValues(incoming, visited_inst) ||
            add_return_value;
    }
  } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(V)) {
    if (V != SI->getValueOperand()) {
      add_return_value =
          collectPossibleReturnValues(SI->getValueOperand(), visited_inst);
      if (!add_return_value) {
        auto current_block =
            framework_function_->getBasicBlock(SI->getParent());
        framework_function_->addPossibleReturnValues(framework_value,
                                                     current_block);
        add_return_value = true;
      }
    }
  } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V)) {
    for (auto *usr : AI->users()) {
      llvm::Value *val = usr;
      add_return_value = collectPossibleReturnValues(usr, visited_inst);
    }
    /* auto current_block = framework_function_->getBasicBlock(AI->getParent());
     */
    /* framework_function_->addPossibleReturnValues(framework_value, */
    /*                                              current_block); */
  } else if (auto *UnaryInst = llvm::dyn_cast<llvm::UnaryInstruction>(V)) {
    add_return_value =
        collectPossibleReturnValues(UnaryInst->getOperand(0), visited_inst);
  } else if (auto *GEI = llvm::dyn_cast<llvm::GetElementPtrInst>(V)) {
    auto current_block = framework_function_->getBasicBlock(GEI->getParent());
    framework_function_->addPossibleReturnValues(framework_value,
                                                 current_block);
  }
  return add_return_value;
}

} // namespace ir_generator

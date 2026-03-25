#include "Checker/FiTx/Core/Function.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Utils.h"

namespace fitx {
std::shared_ptr<fitx::Function>
Function::createManagedFunction(llvm::Function *function,
                                std::unique_ptr<llvm::LoopInfo> loop_info) {
  if (created_functions_.find(function) == created_functions_.end())
    created_functions_[function] =
        std::make_shared<fitx::Function>(function, std::move(loop_info));

  if (!created_functions_[function]->hasLoopInfo() && loop_info)
    created_functions_[function]->setLoopInfo(std::move(loop_info));
  return created_functions_[function];
}

bool Function::IsDebugValueFunction(
    std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.dbg.value");
}

bool Function::IsDebugDeclareFunction(
    std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.dbg.declare");
}

bool Function::IsLifetimeEndFunction(
    std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.lifetime.end");
}

bool Function::IsExpectFunction(std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "llvm.expect");
}

bool Function::IsRefcountDecrementFunction(
    std::shared_ptr<fitx::Function> function) {
  return std::find(refcount_decrement_functions.begin(),
                   refcount_decrement_functions.end(),
                   function->Name()) != refcount_decrement_functions.end();
}

bool Function::IsMemSetFunction(std::shared_ptr<fitx::Function> function) {
  return findFunctionName(function->Name(), "memset");
}

Function::Function(llvm::Function *function,
                   std::unique_ptr<llvm::LoopInfo> loop_info)
    : llvm_function_(function), function_name_(function->getName()),
      return_type_(function->getReturnType()),
      is_definition_(function->isDeclaration()),
      arg_size_(function->arg_size()), loop_info_(std::move(loop_info)),
      contains_loop_back_blocks_(false), return_value_(nullptr),
      protected_refcount_value_(nullptr) {}

std::shared_ptr<fitx::BasicBlock>
Function::getBasicBlock(llvm::BasicBlock *basic_block) {
  auto block =
      std::find(basic_blocks_.begin(), basic_blocks_.end(), basic_block);
  if (block != basic_blocks_.end())
    return *block;

  auto framework_block = std::make_shared<fitx::BasicBlock>(basic_block);
  basic_blocks_.insert(framework_block);

  framework_block->collectPassthroughBlock();

  if (basic_block == &llvm_function_->getEntryBlock())
    init_block_ = framework_block;

  llvm::Loop *loop =
      loop_info_.get() ? loop_info_->getLoopFor(basic_block) : nullptr;

  if (auto *branch_inst =
          llvm::dyn_cast<llvm::BranchInst>(basic_block->getTerminator())) {
    framework_block->setBranchInst(fitx::BranchInst::Create(branch_inst));
  } else if (auto *switch_inst = llvm::dyn_cast<llvm::SwitchInst>(
                 basic_block->getTerminator())) {
    framework_block->setBranchInst(fitx::BranchInst::Create(switch_inst));
  }

  // Generate Predecessor Information
  for (auto *block : llvm::successors(basic_block)) {
    std::shared_ptr<fitx::BasicBlock> successor = getBasicBlock(block);
    framework_block->addSuccessor(successor);

    if (successor->Line() <= framework_block->Line())
      setLoopBackBlock(true);
  }

  // Generate Succesor Information
  for (auto *block : llvm::predecessors(basic_block)) {
    // If we are see a loop header, remove any incoming blocks from the loop
    // itself. This prevents "revisiting the loop blocks" and causing lots of
    // FPs.
    if (loop && loop->getHeader() == basic_block &&
        loop->getBlocksSet().find(block) != loop->getBlocksSet().end())
      continue;

    framework_block->addPredecessor(getBasicBlock(block));
  }

  return framework_block;
}

bool Function::isLoopBlock(std::shared_ptr<fitx::BasicBlock> block) {
  return loop_info_ && loop_info_->getLoopFor(block->LLVMBasicBlock());
}

void Function::setReturnValue(std::shared_ptr<fitx::Value> value) {
  return_value_ = value;
}

std::shared_ptr<fitx::Value> Function::getReturnValue() {
  return return_value_;
}

void Function::setReturnBlock(std::shared_ptr<fitx::BasicBlock> block) {
  return_block_ = block;
}

void Function::addPossibleReturnValues(
    std::shared_ptr<fitx::Value> value,
    std::shared_ptr<fitx::BasicBlock> block) {
  return_assignment_[block] = value;
}

void Function::addCallerFunction(std::shared_ptr<fitx::Function> caller) {
  caller_functions_.insert(caller);
}

const std::set<std::shared_ptr<fitx::Function>> &
Function::CallerFunctions() {
  return caller_functions_;
}

void Function::addRefcountInstruction(
    std::shared_ptr<fitx::Instruction> instruction) {
  if (!last_refcount_call_ || last_refcount_call_ < instruction)
    last_refcount_call_ = instruction;
}

std::shared_ptr<fitx::Instruction> Function::lastRefcountInstruction() {
  return last_refcount_call_;
}

void Function::setLoopBackBlock(bool loop_back) {
  contains_loop_back_blocks_ = loop_back;
}

bool Function::ContainsLoopBackBlock() { return contains_loop_back_blocks_; }

std::map<llvm::Function *, std::shared_ptr<fitx::Function>>
    fitx::Function::created_functions_;
} // namespace fitx

/// \file Function.h
/// \brief FiTx function representation: CFG (blocks, edges), ordered blocks
/// for analysis, and return-value tracking for summaries (paper §4.3).
///
/// Built by Framework_IR::Analyzer from llvm::Function. Holds basic blocks,
/// init/return blocks, possible return assignments per block (for return-code
/// aware propagation), and refcount/lifetime metadata.
#pragma once
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Value.h"

#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace fitx {
/// Linux kernel error macros: used for return-code aware propagation (paper
/// §4.3).
const std::vector<std::string> err_functions{"IS_ERR", "PTR_ERR"};
const std::vector<std::string> refcount_decrement_functions{
    "atomic_dec_and_test", "atomic_dec_and_lock", "refcount_dec_and_test",
    "atomic_dec_return", "atomic_dec_and_lock"};

/// Framework view of an LLVM function: blocks, successors/predecessors,
/// ordered block list for CFG traversal, and return/refcount metadata.
class Function {
public:
  /// Block -> value that may be returned from that block (for summaries).
  using PossibleReturnAssignmentMap =
      std::map<std::shared_ptr<fitx::BasicBlock>,
               std::shared_ptr<fitx::Value>>;

  // Factory method to create and manage functions
  static std::shared_ptr<fitx::Function>
  createManagedFunction(llvm::Function *function,
                        std::unique_ptr<llvm::LoopInfo> loop_info =
                            std::unique_ptr<llvm::LoopInfo>());

  static bool
  IsDebugValueFunction(std::shared_ptr<fitx::Function> function);

  static bool
  IsDebugDeclareFunction(std::shared_ptr<fitx::Function> function);

  static bool
  IsLifetimeEndFunction(std::shared_ptr<fitx::Function> function);

  static bool
  IsRefcountDecrementFunction(std::shared_ptr<fitx::Function> function);

  static bool IsMemSetFunction(std::shared_ptr<fitx::Function> function);

  static bool IsExpectFunction(std::shared_ptr<fitx::Function> function);

  Function(llvm::Function *function, std::unique_ptr<llvm::LoopInfo> loop_info);

  std::shared_ptr<fitx::BasicBlock>
  getBasicBlock(llvm::BasicBlock *basic_block);

  const std::set<std::shared_ptr<fitx::BasicBlock>> &BasicBlocks() {
    return basic_blocks_;
  }

  const std::shared_ptr<fitx::BasicBlock> InitBlock() {
    return init_block_;
  }

  void setReturnValue(std::shared_ptr<fitx::Value> value);
  std::shared_ptr<fitx::Value> getReturnValue();
  void setReturnBlock(std::shared_ptr<fitx::BasicBlock> block);

  void addPossibleReturnValues(std::shared_ptr<fitx::Value> value,
                               std::shared_ptr<fitx::BasicBlock> block);
  const PossibleReturnAssignmentMap &getReturnAssignments() {
    return return_assignment_;
  };

  const llvm::Type *ReturnType() { return return_type_; }
  std::string Name() { return function_name_; };
  bool isDeclaration() { return is_definition_; };
  bool isDebugFunction() {
    return function_name_.find("llvm.dbg") != std::string::npos;
  }

  bool isErrorFunction() {
    return std::find(err_functions.begin(), err_functions.end(),
                     function_name_) != err_functions.end();
  }

  uint64_t ArgSize() { return arg_size_; }

  bool hasLoopInfo() { return loop_info_.get(); }
  bool isLoopBlock(std::shared_ptr<fitx::BasicBlock>);

  void setLoopInfo(std::unique_ptr<llvm::LoopInfo> loop_info) {
    loop_info_ = std::move(loop_info);
  }

  std::shared_ptr<fitx::BasicBlock> ReturnBlock() { return return_block_; }
  static std::map<llvm::Function *, std::shared_ptr<fitx::Function>>
  CreatedFunctions() {
    return created_functions_;
  }

  void addCallerFunction(std::shared_ptr<fitx::Function> caller);
  const std::set<std::shared_ptr<fitx::Function>> &CallerFunctions();

  void
  addRefcountInstruction(std::shared_ptr<fitx::Instruction> instruction);
  std::shared_ptr<fitx::Instruction> lastRefcountInstruction();

  std::shared_ptr<fitx::Value> ProtectedRefcountValue() {
    return protected_refcount_value_;
  }

  void setProtectedRefcountValue(
      std::shared_ptr<fitx::Value> protected_refcount_value) {
    protected_refcount_value_ = protected_refcount_value;
  }

  void setLoopBackBlock(bool loop_back);
  bool ContainsLoopBackBlock();

  void
  addOrderedBlock(std::vector<std::shared_ptr<fitx::BasicBlock>> &block) {
    ordered_basic_blocks_ = block;
  }

  const std::vector<std::shared_ptr<fitx::BasicBlock>> &
  OrderedBasicBlocks() {
    return ordered_basic_blocks_;
  }

private:
  static std::map<llvm::Function *, std::shared_ptr<fitx::Function>>
      created_functions_;

  // Function metas these should be updated to copy llvm::Function, but for the
  // time being, we manually copy everything
  llvm::Function *llvm_function_;

  const llvm::Type *return_type_;

  std::unique_ptr<llvm::LoopInfo> loop_info_;
  std::string function_name_;
  bool is_definition_;
  uint64_t arg_size_;

  std::shared_ptr<fitx::Value> return_value_;

  bool contains_loop_back_blocks_;

  PossibleReturnAssignmentMap return_assignment_;

  std::set<std::shared_ptr<fitx::Function>> caller_functions_;
  std::set<std::shared_ptr<fitx::BasicBlock>> basic_blocks_;

  // TODO: Update to ordered basic block after it is proven worthy
  std::vector<std::shared_ptr<fitx::BasicBlock>> ordered_basic_blocks_;

  std::shared_ptr<fitx::BasicBlock> init_block_;
  std::shared_ptr<fitx::BasicBlock> return_block_;

  // Refcount protected value related features
  std::shared_ptr<fitx::Value> protected_refcount_value_;
  std::shared_ptr<fitx::Instruction> last_refcount_call_;
};
} // namespace fitx

/// \file BasicBlock.hpp
/// FiTx CFG basic block: predecessors, successors, instructions.
/// Used by CFG-based typestate analysis (paper §4.2).
#pragma once
#include "llvm/IR/BasicBlock.h"

#include "Checker/FiTx/Core/Instruction.h"

#include <memory>
#include <set>
#include <vector>

namespace fitx {
// TODO: remove this prototype from here
class Function;
class BranchInst;
class BasicBlock {
public:
  constexpr static long kNoId = -1;
  BasicBlock(llvm::BasicBlock *basic_block);

  bool operator==(llvm::BasicBlock *basic_block);
  bool operator==(const fitx::BasicBlock &basic_block);

  bool operator<(const fitx::BasicBlock &basic_block);

  friend bool
  operator<(const std::weak_ptr<fitx::BasicBlock> basic_block,
            const std::weak_ptr<fitx::BasicBlock> target_block);

  friend bool
  operator==(const std::shared_ptr<fitx::BasicBlock> basic_block,
             const llvm::BasicBlock *llvm_block);

  void addSuccessor(std::shared_ptr<fitx::BasicBlock> block);
  void addPredecessor(std::shared_ptr<fitx::BasicBlock> block);
  bool isInPredecessor(std::shared_ptr<fitx::BasicBlock> block);

  const std::set<std::weak_ptr<fitx::BasicBlock>> &Predecessors() {
    return predecessors_;
  }

  const std::set<std::weak_ptr<fitx::BasicBlock>> &Successors() {
    return successors_;
  }

  void addInstruction(std::shared_ptr<fitx::Instruction> inst);
  const std::vector<std::shared_ptr<fitx::Instruction>> Instructions() {
    return instructions_;
  }

  llvm::BasicBlock *LLVMBasicBlock() { return llvm_basic_block_; }
  const std::string &Name() { return name_; }

  std::weak_ptr<fitx::Function> Parent() { return parent_; };

  bool isCleanupBlock() { return !pass_through_.empty(); };
  bool collectPassthroughBlock();
  const std::vector<std::weak_ptr<fitx::BasicBlock>>
  getPassthroughBlock(std::weak_ptr<fitx::BasicBlock> succ_block);

  void addDeadValue(std::shared_ptr<fitx::Value> value);
  /// Compare by pointer address only to avoid invoking Value::operator<
  /// during set operations (can crash with default comparator).
  struct ValuePtrCompare {
    bool operator()(const std::shared_ptr<fitx::Value> &a,
                    const std::shared_ptr<fitx::Value> &b) const {
      return a.get() < b.get();
    }
  };
  const std::set<std::shared_ptr<fitx::Value>> DeadValues() const;

  void setBranchInst(std::shared_ptr<fitx::BranchInst> branch_inst) {
    branch_inst_ = branch_inst;
  }

  std::shared_ptr<fitx::BranchInst> getBranchInst() {
    return branch_inst_;
  }

  unsigned int Line() { return line_; }

  void setId(long id) { id_ = id; }
  long Id() { return id_; }

private:
  // BasicBlockInfo
  long id_;
  llvm::BasicBlock *llvm_basic_block_;
  std::string name_;
  unsigned int line_;

  // Contained Instructions
  std::vector<std::shared_ptr<fitx::Instruction>> instructions_;
  std::weak_ptr<fitx::Function> parent_;

  std::shared_ptr<fitx::BranchInst> branch_inst_;

  bool is_cleanup_block_;

  std::map<std::weak_ptr<fitx::BasicBlock>,
           std::vector<std::weak_ptr<fitx::BasicBlock>>,
           std::owner_less<std::weak_ptr<fitx::BasicBlock>>>
      pass_through_;

  // Values to be dead by the end of this BB
  std::set<std::shared_ptr<fitx::Value>, ValuePtrCompare> dead_values_;

  // Interactions
  std::set<std::weak_ptr<fitx::BasicBlock>> predecessors_;
  std::set<std::weak_ptr<fitx::BasicBlock>> successors_;
};
} // namespace fitx

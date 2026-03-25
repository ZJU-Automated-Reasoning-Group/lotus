/**
 * \file ExpandAssume.h
 * \brief Pass for expanding assume intrinsics into explicit control flow
 * \author Adapted from pagai for Lotus framework
 */
#ifndef LOTUS_TRANSFORM_EXPAND_ASSUME_H
#define LOTUS_TRANSFORM_EXPAND_ASSUME_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Pass.h"

#include <set>

namespace lotus {

/**
 * \class ExpandAssume
 * \brief Pass for expanding assume intrinsics into explicit conditional
 * branches
 *
 * LLVM's assume intrinsics are used to provide hints to the optimizer about
 * conditions that are expected to be true. This pass converts these intrinsics
 * into explicit conditional branches that can be analyzed by static analysis
 * tools, making the assumptions visible in the control flow graph.
 */
class ExpandAssume : public llvm::FunctionPass {
private:
  /**
   * \brief Set of already processed assume calls to avoid infinite loops
   */
  std::set<llvm::Value *> processedAssumes;

  /**
   * \brief Check if an instruction is an assume intrinsic call
   * \param inst The instruction to check
   * \return true if the instruction is an assume call
   */
  bool isAssumeCall(llvm::Instruction *inst);

  /**
   * \brief Get the condition from an assume call
   * \param assumeCall The assume call instruction
   * \return The condition value passed to assume
   */
  llvm::Value *getAssumeCondition(llvm::CallInst *assumeCall);

  /**
   * \brief Create a conditional branch based on an assume condition
   * \param condition The condition to branch on
   * \param originalBlock The block containing the assume call
   * \param insertBefore The instruction to insert branches before
   * \return The terminator instruction for the split block
   */
  llvm::Instruction *createConditionalBranch(llvm::Value *condition,
                                             llvm::BasicBlock *originalBlock,
                                             llvm::Instruction *insertBefore);

  /**
   * \brief Split a basic block and insert conditional branch
   * \param condition The condition for the branch
   * \param splitBefore The instruction to split before
   * \param unreachable Whether the false branch should be unreachable
   * \return The terminator instruction for the split block
   */
  llvm::Instruction *splitBlockAndInsertIfThen(llvm::Value *condition,
                                               llvm::Instruction *splitBefore,
                                               bool unreachable = false);

  /**
   * \brief Process a single function for assume expansion
   * \param function The function to process
   * \return true if any transformations were made
   */
  bool processFunction(llvm::Function &function);

  /**
   * \brief Process a single assume call instruction
   * \param assumeCall The assume call to process
   * \return true if the assume was expanded
   */
  bool processAssumeCall(llvm::CallInst *assumeCall);

public:
  static char ID;

  /**
   * \brief Constructor
   */
  ExpandAssume();

  /**
   * \brief Run the pass on a function
   * \param function The function to analyze
   * \return true if the function was modified
   */
  bool runOnFunction(llvm::Function &function) override;

  /**
   * \brief Get analysis usage information
   * \param AU Analysis usage object to populate
   */
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  /**
   * \brief Get the number of assume calls processed
   * \return Number of assume calls that were expanded
   */
  size_t getProcessedAssumeCount() const;

  /**
   * \brief Reset the processed assumes counter
   */
  void resetProcessedCount();
};

} // namespace lotus

#endif // LOTUS_TRANSFORM_EXPAND_ASSUME_H

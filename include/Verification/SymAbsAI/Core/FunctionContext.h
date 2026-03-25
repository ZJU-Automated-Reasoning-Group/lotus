/**
 * @file FunctionContext.h
 * @brief Per-function SMT encoding context for SymAbsAI, providing
 * access to represented LLVM values, memory model, and path/edge predicates.
 */
#pragma once

#include "Verification/SymAbsAI/Core/InstructionSemantics.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <llvm/ADT/iterator_range.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <z3++.h>

namespace symabs_ai {
class MemoryModel;
class FloatingPointModel;
class DomainConstructor;
class RepresentedValue;
class ModuleContext;

namespace configparser {
class Config;
}

/**
 * Provides formulas for acyclic subgraphs of a function's control flow graph.
 *
 * Typical use of this class would involve creating its instance for a
 * particular fragment and then calling formulaForFunction(). The class also
 * exposes some useful utility functions that aid implementations of abstract
 * domains in creating SMT formulas.
 *
 * @todo define what a fragment is and implement this for fragments
 */
class FunctionContext {
private:
  const ModuleContext *ModuleContext_;
  llvm::Function *Function_;
  std::vector<RepresentedValue> RepresentedValues_;
  z3::expr UndefinedBehaviorFlag_;
  std::unique_ptr<MemoryModel> MemoryModel_;
  std::unique_ptr<FloatingPointModel> FloatingPointModel_;
  configparser::Config Config_;
  mutable std::unique_ptr<llvm::DominatorTreeWrapperPass> DominatorTreePass_;

  /**
   * Returns a reference to the DominatorTree of the function.
   *
   * The DominatorTree is created when this function is called for the first
   * time.
   */
  llvm::DominatorTree &getDomTree() const;

  /**
   * Returns a formula that is true iff the non-PHI part of the given
   * basic block is executed during the concrete run.
   */
  z3::expr getBasicBlockBodyCondition(const Fragment &frag,
                                      llvm::BasicBlock *bb) const;

  /**
   * Returns a formula that is true iff the PHI part of the given basic
   * block is executed during the concrete run.
   */
  z3::expr getBasicBlockPhiCondition(const Fragment &frag,
                                     llvm::BasicBlock *bb) const;

  FunctionContext(const FunctionContext &) = delete;
  FunctionContext &operator=(const FunctionContext &) = delete;

public:
  // have to be defined in FunctionContext.cpp since sizeof(MemoryModel) is
  // unknown
  FunctionContext(FunctionContext &&other);
  FunctionContext &operator=(FunctionContext &&other);

  FunctionContext(llvm::Function *func, const ModuleContext *mctx);

  // Two-phase initialization required since memory models need reference
  // to FunctionContext.
  void setMemoryModel(unique_ptr<MemoryModel> mem_model);

  z3::expr getEdgeVariable(llvm::BasicBlock *bb_from,
                           llvm::BasicBlock *bb_to) const;

  int getPointerSize() const;

  /**
   * Returns a z3 expression that is true if the chosen path contains
   * computations that immediately lead to undefined behavior (e.g. division
   * by zero).
   * Might be undefined by a model if no such computation is performed.
   */
  z3::expr getUndefinedBehaviorFlag() const;

  /**
   * Constructs the basic semantic formula for the represented fragment. It
   * will be true iff all program variables and edge indicator variables
   * describe a valid run of the program.
   */
  z3::expr formulaFor(const Fragment &frag) const;

  RepresentedValue *findRepresentedValue(const llvm::Value *value) const;

  /**
   * Determines whether a given value is represented in the SMT world as a
   * variable.
   */
  bool isRepresentedValue(llvm::Value *value) const {
    return findRepresentedValue(value) != nullptr;
  }

  const std::vector<RepresentedValue> &representedValues() const {
    return RepresentedValues_;
  }

  const std::vector<RepresentedValue> parameters() const;

  std::vector<RepresentedValue> valuesAvailableIn(llvm::BasicBlock *bb,
                                                  bool after) const;

  z3::sort sortForType(llvm::Type *type) const;

  unsigned int bitsForType(llvm::Type *type) const;

  /**
   * Returns a Z3 context used for creation of all formulas.
   */
  z3::context &getZ3() const;

  /**
   * Returns the llvm Function whose semantics are described.
   */
  llvm::Function *getFunction() const { return Function_; }

  const MemoryModel &getMemoryModel() const { return *MemoryModel_; }
  const FloatingPointModel &getFloatingPointModel() const {
    return *FloatingPointModel_;
  }

  const configparser::Config getConfig() const;

  const ModuleContext &getModuleContext() const { return *ModuleContext_; }

  // non-default destructor needed for unique_ptr on incomplete type
  ~FunctionContext();
};
} // namespace symabs_ai

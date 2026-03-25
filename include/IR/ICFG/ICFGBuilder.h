/// @file ICFGBuilder.h
/// @brief Builder class for constructing ICFG from LLVM modules.

#pragma once

#include "IR/ICFG/ICFG.h"

/// @brief Constructs an ICFG from an LLVM module.
///
/// Processes all functions in a module to build intraprocedural and
/// interprocedural control flow edges.
class ICFGBuilder {
private:
  ICFG *icfg;

public:
  /// @brief Constructs an ICFG builder.
  /// @param i ICFG to populate.
  ICFGBuilder(ICFG *i) : icfg(i) {}

  /// @brief Builds the ICFG for all functions in the module.
  /// @param module LLVM module to process.
  void build(llvm::Module *module);

  bool _removeCycleAfterBuild =
      false; ///< Flag to remove cycles after building.

public:
  /// @brief Sets whether to remove cycles after building the ICFG.
  /// @param b True to remove cycles.
  void setRemoveCycleAfterBuild(bool b);

private:
  /// @brief Processes a single function to create ICFG nodes and edges.
  /// @param func Function to process.
  void processFunction(const llvm::Function *func);

  /// @brief Gets or creates an ICFG node for a basic block.
  /// @param bb Basic block.
  /// @return Pointer to the ICFG node.
  IntraBlockNode *getOrAddIntraBlockICFGNode(const llvm::BasicBlock *bb) {
    return icfg->getIntraBlockNode(bb);
  }

  /// @brief Removes interprocedural back edges (recursive calls).
  void removeInterCallCycle();

  /// @brief Removes intraprocedural back edges (loops).
  void removeIntraBlockCycle();
};

/// \file Framework.hpp
/// FiTx framework pass: generates daily development-friendly bug checkers.
/// Based on: Suzuki et al., "Balancing Analysis Time and Bug Detection: Daily
/// Development-friendly Bug Detection in Linux", USENIX ATC 2024.
/// See docs/source/checker/fitx.rst and the paper Section 4 (FiTx design).

#pragma once
#include "llvm/IR/CFG.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

#include "Checker/FiTx/Frontend/State.h"

#include <string>
#include <vector>

namespace fitx {

/// Main FiTx pass: runs typestate-based bug checkers per compilation unit.
/// Each checker is defined by a StateManager (typestate FSM); the pass
/// traverses the CFG path-insensitively with return-code aware propagation
/// (paper Section 4.2, 4.3).
class FrameworkPass : public llvm::ModulePass {
public:
  static char ID;
  static std::vector<fitx::FrameworkPass *> passes;

  FrameworkPass();
  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  /// Entry: define typestate checkers (defineStates) and run analysis (paper
  /// §4).
  bool runOnModule(llvm::Module &M) override;

  /// Override to define states and transitions for a bug pattern (paper §4.1,
  /// Table 5).
  virtual void defineStates() {};
  void createTransitions(fitx::StateManager &manager);

  void addStateManager(fitx::StateManager manager) {
    manager_.push_back(manager);
  }

private:
  std::vector<fitx::StateManager> manager_;
}; // end of struct
} // namespace fitx

/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: DyckAA adapter for NullPointer analyses
 */

#include "Analysis/NullPointer/AliasAnalysisAdapter.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;

// Implementation of DyckAAAdapter
DyckAAAdapter::DyckAAAdapter(Module *M, const DyckAliasAnalysis *DAA)
    : ModuleRef(M), DyckAA(DAA) {}

DyckAAAdapter::~DyckAAAdapter() {
  // No dynamic allocation, nothing to clean up
}

bool DyckAAAdapter::mayAlias(Value *V1, Value *V2, Instruction * /*InstPoint*/,
                             bool /*IncludeI*/) {
  // Use the actual DyckAA results
  if (!DyckAA) {
    // Conservative fallback if DyckAA is not available
    return true;
  }

  // For instruction points, we ignore the instruction point for now
  // since DyckAA is context-insensitive
  return DyckAA->mayAlias(V1, V2);
}

bool DyckAAAdapter::mayNull(Value *V, Instruction * /*InstPoint*/,
                            bool /*BeforeInstruction*/) {
  // Use the actual DyckAA results
  if (!DyckAA) {
    // Conservative fallback if DyckAA is not available
    return true;
  }

  // For instruction points, we ignore the instruction point for now
  // since DyckAA is context-insensitive
  return DyckAA->mayNull(V);
}

// Factory method to create the appropriate adapter
// The caller takes ownership of the returned object
AliasAnalysisAdapter *
AliasAnalysisAdapter::createAdapter(Module *M, const DyckAliasAnalysis *DAA) {
  return new DyckAAAdapter(M, DAA);
}
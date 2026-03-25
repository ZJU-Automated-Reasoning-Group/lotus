// Implementation of TransferFunction.
//
// This file contains the implementation of the transfer function dispatcher.
// It acts as the visitor for CFG nodes, invoking the specific evaluation logic
// for each node type (Entry, Alloc, Copy, etc.).
//
// The evaluation produces an EvalResult, which contains the set of successors
// to visit next, along with any updates to the Store.

#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"

#include "Alias/TPA/Util/IO/PointerAnalysis/Printer.h"

#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

using namespace context;
using namespace llvm;

namespace tpa {

// Adds successors that are "top-level" (Environment only).
// These successors inherit the Context but don't carry a Store update directly
// in the way mem-level nodes do (though they might share the global Env).
void TransferFunction::addTopLevelSuccessors(const ProgramPoint &pp,
                                             EvalResult &evalResult) {
  for (auto *const succ : pp.getCFGNode()->uses())
    evalResult.addTopLevelProgramPoint(ProgramPoint(pp.getContext(), succ));
}

// Adds successors that are "memory-level" (Store consumers).
// These successors must receive the current (possibly updated) Store.
void TransferFunction::addMemLevelSuccessors(const ProgramPoint &pp,
                                             const Store &store,
                                             EvalResult &evalResult) {
  for (auto *const succ : pp.getCFGNode()->succs())
    evalResult.addMemLevelProgramPoint(ProgramPoint(pp.getContext(), succ),
                                       store);
}

// Main evaluation dispatch loop.
// Delegates to specific node handlers based on the CFGNodeTag.
// Returns an EvalResult containing next steps for the Propagator.
EvalResult TransferFunction::eval(const ProgramPoint &pp) {
  // errs() << "Evaluating " << pp.getCFGNode()->getFunction().getName() << "::"
  // << pp << "\n";
  EvalResult evalResult;

  switch (pp.getCFGNode()->getNodeTag()) {
  case CFGNodeTag::Entry: {
    // Initialize function parameters in the Env
    evalEntryNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Alloc: {
    // Handle memory allocation (malloc, alloca, new)
    evalAllocNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Copy: {
    // Handle pointer assignment/copy (p = q)
    evalCopyNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Offset: {
    // Handle address computation (GEP)
    evalOffsetNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Load: {
    // Handle load from memory (p = *q)
    // Requires local Store state
    if (localState != nullptr)
      evalLoadNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Store: {
    // Handle store to memory (*p = q)
    // Requires local Store state
    if (localState != nullptr)
      evalStoreNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Call: {
    // Handle function calls
    // Requires local Store state
    if (localState != nullptr)
      evalCallNode(pp, evalResult);
    break;
  }
  case CFGNodeTag::Ret: {
    // Handle return instructions
    // Requires local Store state
    if (localState != nullptr)
      evalReturnNode(pp, evalResult);
    break;
  }
  }

  return evalResult;
}

} // namespace tpa

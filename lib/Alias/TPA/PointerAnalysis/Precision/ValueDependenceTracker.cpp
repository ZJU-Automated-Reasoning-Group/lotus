// Implementation of the ValueDependenceTracker.
//
// This component is responsible for computing the backward data-flow
// dependencies for a given program point. It answers the question: "Where did
// the value at this point come from?"
//
// Usage:
// Used by the PrecisionLossTracker to traverse the value graph backwards.
//
// Logic:
// It visits the CFG node associated with the ProgramPoint and identifies
// its operands (definitions).
// - Copy/Offset: Depends on the source operand.
// - Call: Depends on the return values of all possible callees.
// - Entry: Depends on the arguments passed by all callers.
// - Return: Depends on the definitions reaching the return node.

#include "Alias/TPA/PointerAnalysis/Precision/ValueDependenceTracker.h"

#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFGNode.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/NodeVisitor.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"

#include <llvm/IR/Function.h>

using namespace context;
using namespace llvm;

namespace tpa {

namespace {

// Visitor to extract dependencies based on node type.
class ValueTracker : public ConstNodeVisitor<ValueTracker> {
private:
  const SemiSparseProgram &ssProg;

  using CallGraphType = CallGraph<ProgramPoint, FunctionContext>;
  const CallGraphType &callGraph;

  const Context *ctx;
  ProgramPointSet &ppSet;

  // Helper to find the defining node for an LLVM Value and add it to the set.
  void addDef(const CFGNode &node, const Value *val) {
    assert(val != nullptr);

    // Global values don't have a defining CFG node in the function (they are
    // constant/static).
    if (isa<GlobalValue>(val))
      return;

    const auto *predNode = node.getCFG().getCFGNodeForValue(val);
    assert(predNode != nullptr);
    ppSet.insert(ProgramPoint(ctx, predNode));
  }

public:
  ValueTracker(const SemiSparseProgram &s, const CallGraphType &cg,
               const Context *c, ProgramPointSet &p)
      : ssProg(s), callGraph(cg), ctx(c), ppSet(p) {}

  // Entry Node: Depends on arguments from all call sites calling this function.
  void visitEntryNode(const EntryCFGNode &entryNode) {
    auto const &func = entryNode.getFunction();
    auto callers = callGraph.getCallers(FunctionContext(ctx, &func));
    for (auto const &caller : callers)
      ppSet.insert(caller);
  }

  // Copy Node: Depends on all source values.
  void visitCopyNode(const CopyCFGNode &copyNode) {
    for (const auto *src : copyNode)
      addDef(copyNode, src);
  }

  // Offset Node: Depends on the base pointer.
  void visitOffsetNode(const OffsetCFGNode &offsetNode) {
    addDef(offsetNode, offsetNode.getSrc());
  }

  // Call Node: Depends on the return values of all resolved callees.
  void visitCallNode(const CallCFGNode &callNode) {
    auto callees = callGraph.getCallees(ProgramPoint(ctx, &callNode));
    for (auto const &callee : callees) {
      const auto *cfg = ssProg.getCFGForFunction(*callee.getFunction());
      assert(cfg != nullptr);

      // If the function returns, add its exit node (ReturnNode) as a
      // dependency.
      if (!cfg->doesNotReturn()) {
        const auto *retNode = cfg->getExitNode();
        ppSet.insert(ProgramPoint(callee.getContext(), retNode));
      }
    }
  }

  // Return Node: Depends on the value being returned.
  // Note: ReturnCFGNode usually has a 'def' set which tracks the reaching
  // definition of the return value? Or maybe it tracks the value operand
  // itself? Looking at CFG.h/cpp, ReturnCFGNode seems to hold the return value.
  // 'defs()' likely returns the node defining that value (via def-use chain
  // logic?).
  void visitReturnNode(const ReturnCFGNode &retNode) {
    for (auto *defNode : retNode.defs())
      ppSet.insert(ProgramPoint(ctx, defNode));
  }

  // These nodes are no-ops for *value* dependence (they affect memory, not the
  // top-level pointer value being tracked). Alloc: Is a source, depends on
  // nothing (except size, irrelevant here). Load: Depends on memory content,
  // handled separately? Or treated as a source? Store: Affects memory, doesn't
  // produce a value.
  void visitAllocNode(const AllocCFGNode &) {}
  void visitLoadNode(const LoadCFGNode &) {}
  void visitStoreNode(const StoreCFGNode &) {}
};

} // namespace

// Main entry point.
ProgramPointSet
ValueDependenceTracker::getValueDependencies(const ProgramPoint &pp) const {
  ProgramPointSet ppSet;

  const auto *ctx = pp.getContext();
  const auto *node = pp.getCFGNode();

  ValueTracker(ssProg, callGraph, ctx, ppSet).visit(*node);

  return ppSet;
}

} // namespace tpa

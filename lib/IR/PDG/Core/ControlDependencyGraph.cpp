/**
 * @file ControlDependencyGraph.cpp
 * @brief Implementation of the control dependency analysis for the PDG
 *
 * This file implements the ControlDependencyGraph pass, which analyzes control
 * dependencies between program elements. Control dependencies occur when the
 * execution of one instruction determines whether another instruction executes.
 *
 * Key features:
 * - Analysis of branch instructions and their targets
 * - Construction of post-dominator trees for control flow analysis
 * - Function-level control dependency analysis
 * - Integration with the overall PDG framework
 * - Support for different types of control dependencies
 *
 * The control dependency analysis is a fundamental component of the PDG system,
 * complementing data dependency analysis to provide a complete view of
 * program dependencies.
 */

#include "IR/PDG/Core/ControlDependencyGraph.h"

char pdg::ControlDependencyGraph::ID = 0;

using namespace llvm;
bool pdg::ControlDependencyGraph::runOnFunction(Function &F) {
  _PDT = &getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
  addControlDepFromEntryNodeToInsts(F);
  addControlDepFromDominatedBlockToDominator(F);
  addControlDepFromIndirectBranches(F);
  return false;
}

void pdg::ControlDependencyGraph::addControlDepFromNodeToBB(
    Node &n, BasicBlock &BB, EdgeType edge_type) {
  ProgramGraph &g = ProgramGraph::getInstance();
  for (auto &inst : BB) {
    Node *inst_node = g.getNode(inst);
    // TODO: a special case when gep is used as a operand in load. Fix later
    if (inst_node != nullptr)
      n.addNeighbor(*inst_node, edge_type);
    // assert(inst_node != nullptr && "cannot find node for inst\n");
  }
}

void pdg::ControlDependencyGraph::addControlDepFromEntryNodeToInsts(
    Function &F) {
  ProgramGraph &g = ProgramGraph::getInstance();
  FunctionWrapper *func_w = g.getFuncWrapperMap()[&F];
  if (!func_w)
    return;
  for (auto &BB : F) {
    addControlDepFromNodeToBB(*func_w->getEntryNode(), BB,
                              EdgeType::CONTROLDEP_ENTRY);
  }
}

void pdg::ControlDependencyGraph::addControlDepFromDominatedBlockToDominator(
    Function &F) {
  // Implements the standard Ferrante/Ottenstein/Warren CDG algorithm:
  // For each CFG edge (A -> B) where B does not post-dominate A, walk up the
  // post-dominator tree from B to the parent of A in the post-dominator tree,
  // adding CONTROLDEP_BR edges from A's terminator to every block on that path.
  //
  // Fix: the original code added a direct edge to succ_bb AND then walked the
  // post-dominator tree from succ_bb upward, which double-counted succ_bb when
  // nearestCommonDominator == &BB.  The corrected version only uses the tree
  // walk (which already includes succ_bb as the starting point).
  //
  // IndirectBr is handled separately in addControlDepFromIndirectBranches().
  ProgramGraph &g = ProgramGraph::getInstance();
  for (auto &BB : F) {
    Instruction *terminator = BB.getTerminator();
    if (!terminator)
      continue;
    // Skip blocks with 0 or 1 successors (no branching decision).
    // Also skip IndirectBr — handled by addControlDepFromIndirectBranches().
    if (terminator->getNumSuccessors() <= 1)
      continue;
    if (isa<IndirectBrInst>(terminator))
      continue;

    Node *terminator_node = g.getNode(*terminator);
    if (terminator_node == nullptr)
      continue;

    for (auto succ_iter = succ_begin(&BB); succ_iter != succ_end(&BB);
         succ_iter++) {
      BasicBlock *succ_bb = *succ_iter;
      // Skip self-loops: a block cannot be control-dependent on itself.
      if (succ_bb == &BB)
        continue;
      // Only process edges where succ_bb does NOT post-dominate BB.
      if (_PDT->dominates(succ_bb, &BB))
        continue;

      // Walk up the post-dominator tree from succ_bb to (but not including)
      // the nearest common post-dominator of BB and succ_bb.
      BasicBlock *ncd = _PDT->findNearestCommonDominator(&BB, succ_bb);
      auto *ncd_node = _PDT->getNode(ncd);
      for (auto *cur = _PDT->getNode(succ_bb); cur && cur != ncd_node;
           cur = cur->getIDom()) {
        addControlDepFromNodeToBB(*terminator_node, *cur->getBlock(),
                                  EdgeType::CONTROLDEP_BR);
      }
    }
  }
}

void pdg::ControlDependencyGraph::addControlDepFromIndirectBranches(
    Function &F) {
  // Handle IndirectBrInst: emit CONTROLDEP_IND_BR edges using the same
  // post-dominator-tree walk as for conditional branches.
  ProgramGraph &g = ProgramGraph::getInstance();
  for (auto &BB : F) {
    auto *terminator = BB.getTerminator();
    if (!terminator)
      continue;
    auto *ind_br = dyn_cast<IndirectBrInst>(terminator);
    if (!ind_br)
      continue;

    Node *terminator_node = g.getNode(*terminator);
    if (terminator_node == nullptr)
      continue;

    for (unsigned i = 0; i < ind_br->getNumSuccessors(); ++i) {
      BasicBlock *succ_bb = ind_br->getSuccessor(i);
      if (succ_bb == &BB)
        continue;
      if (_PDT->dominates(succ_bb, &BB))
        continue;

      BasicBlock *ncd = _PDT->findNearestCommonDominator(&BB, succ_bb);
      auto *ncd_node = _PDT->getNode(ncd);
      for (auto *cur = _PDT->getNode(succ_bb); cur && cur != ncd_node;
           cur = cur->getIDom()) {
        addControlDepFromNodeToBB(*terminator_node, *cur->getBlock(),
                                  EdgeType::CONTROLDEP_IND_BR);
      }
    }
  }
}

void pdg::ControlDependencyGraph::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.setPreservesAll();
}

static RegisterPass<pdg::ControlDependencyGraph>
    CDG("cdg", "Control Dependency Graph Construction", false, true);

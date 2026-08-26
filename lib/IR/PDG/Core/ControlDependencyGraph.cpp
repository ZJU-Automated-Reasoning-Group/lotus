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
 * - Reuse of Lotus's standard control-dependence analysis
 * - Function-level control dependency analysis
 * - Integration with the overall PDG framework
 * - Support for different types of control dependencies
 *
 * The control dependency analysis is a fundamental component of the PDG system,
 * complementing data dependency analysis to provide a complete view of
 * program dependencies.
 */

#include "IR/PDG/Core/ControlDependencyGraph.h"

#include "Analysis/ControlDependence/ControlDependence.h"

char pdg::ControlDependencyGraph::ID = 0;

using namespace llvm;
bool pdg::ControlDependencyGraph::runOnFunction(Function &F) {
  addControlDepFromEntryNodeToInsts(F);
  addStandardControlDependencies(F);
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

void pdg::ControlDependencyGraph::addStandardControlDependencies(Function &F) {
  ProgramGraph &g = ProgramGraph::getInstance();
  lotus::cd::ControlDependenceAnalysis analysis(
      F, {lotus::cd::Algorithm::Standard});

  for (BasicBlock &BB : F) {
    Instruction *terminator = BB.getTerminator();
    if (!terminator || terminator->getNumSuccessors() <= 1)
      continue;

    Node *terminator_node = g.getNode(*terminator);
    if (!terminator_node)
      continue;

    EdgeType edge_type = isa<IndirectBrInst>(terminator)
                             ? EdgeType::CONTROLDEP_IND_BR
                             : EdgeType::CONTROLDEP_BR;
    for (const BasicBlock *dependent : analysis.getDependents(&BB)) {
      // Preserve the PDG convention that a branch does not control itself.
      if (dependent == &BB)
        continue;
      addControlDepFromNodeToBB(
          *terminator_node, *const_cast<BasicBlock *>(dependent), edge_type);
    }
  }
}

void pdg::ControlDependencyGraph::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

static RegisterPass<pdg::ControlDependencyGraph>
    CDG("cdg", "Control Dependency Graph Construction", false, true);

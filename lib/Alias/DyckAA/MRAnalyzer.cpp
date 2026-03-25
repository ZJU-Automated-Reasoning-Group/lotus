/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Alias/DyckAA/MRAnalyzer.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

MRAnalyzer::MRAnalyzer(Module *M, DyckGraph *DG, DyckCallGraph *DCG)
    : M(M), DG(DG), DCG(DCG) {}

MRAnalyzer::~MRAnalyzer() = default;

void MRAnalyzer::intraProcedureAnalysis() {}

void MRAnalyzer::interProcedureAnalysis() {
  // fixme let's impl a simple version where we do not use scc and bottom-up
  // inter-proc analysis
  for (auto It = DCG->nodes_begin(), E = DCG->nodes_end(); It != E; ++It)
    runOnFunction(*It);
}

void MRAnalyzer::runOnFunction(DyckCallGraphNode *CGNode) {
  auto *F = CGNode->getLLVMFunction();
  if (!F)
    return; // there is one and only one fake node that does not include a
            // function

  auto &MR = Func2MR[F];
  std::set<DyckGraphNode *> &Refs = MR.Mods;
  std::set<DyckGraphNode *> &Mods = MR.Refs;

  // compute a set of dyck nodes reachable from parameters and todo returns
  std::set<DyckGraphNode *> ParReachableNodes;
  std::set<DyckGraphNode *> RetReachableNodes;
  for (unsigned K = 0; K < F->arg_size(); ++K) {
    auto *DGNode = DG->findDyckVertex(F->getArg(K));
    if (!DGNode)
      continue;
    DG->getReachableVertices(DGNode, ParReachableNodes);
  }
  for (unsigned K = 0; K < F->arg_size(); ++K) {
    auto *DGNode = DG->findDyckVertex(F->getArg(K));
    if (!DGNode)
      continue;
    ParReachableNodes.erase(DGNode); // let us exclude explicit parameters
  }

  // for each instruction,
  // if it refs a node that is reachable from parameters add it to refs
  // if it mods a node that is reachable from parameters add it to mods
  for (auto &I : instructions(F)) {
    for (unsigned K = 0; K < I.getNumOperands(); ++K) {
      auto *Ref = I.getOperand(K);
      auto *RefNode = DG->findDyckVertex(Ref);
      if (!RefNode)
        continue;
      // check if reachable from parameters
      if (ParReachableNodes.count(RefNode))
        Refs.insert(RefNode);
    }
    if (F->onlyReadsMemory())
      continue; // a read only function
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      auto *Ptr = SI->getPointerOperand();
      auto *PtrNode = DG->findDyckVertex(Ptr);
      if (!PtrNode)
        continue;
      auto *ModNode = PtrNode->getOutVertex(DG->getDereferenceEdgeLabel());
      // check if reachable from parameters and returns
      if (ParReachableNodes.count(ModNode) || RetReachableNodes.count(ModNode))
        Mods.insert(ModNode);
    } else {
      // todo other instructions that may revise a memory
    }
  }
}

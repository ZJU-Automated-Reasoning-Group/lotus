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

#include "Alias/DyckAA/DyckValueFlowAnalysis.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Utils/LLVM/RecursiveTimer.h"

char DyckValueFlowAnalysis::ID = 0;
static RegisterPass<DyckValueFlowAnalysis>
    X("dyckvfa", "vfa based on the unification based alias analysis");

DyckValueFlowAnalysis::DyckValueFlowAnalysis() : ModulePass(ID) {
  VFG = nullptr;
}

DyckValueFlowAnalysis::~DyckValueFlowAnalysis() { delete VFG; }

void DyckValueFlowAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<DyckAliasAnalysis>();
  AU.addRequired<DyckModRefAnalysis>();
}

DyckVFG *DyckValueFlowAnalysis::getDyckVFGraph() const { return VFG; }

bool DyckValueFlowAnalysis::runOnModule(Module &M) {
  RecursiveTimer DyckVFA("Running DyckVFA");
  auto *DyckAA = &getAnalysis<DyckAliasAnalysis>();
  auto *DyckMRA = &getAnalysis<DyckModRefAnalysis>();
  VFG = new DyckVFG(DyckAA, DyckMRA, &M);
  return false;
}

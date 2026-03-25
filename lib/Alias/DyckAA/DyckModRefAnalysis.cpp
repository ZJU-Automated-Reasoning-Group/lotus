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

#include "Alias/DyckAA/DyckModRefAnalysis.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/MRAnalyzer.h"
#include "Utils/LLVM/RecursiveTimer.h"

char DyckModRefAnalysis::ID = 0;
static RegisterPass<DyckModRefAnalysis>
    X("dyckmr", "m/r based on the unification based alias analysis");

DyckModRefAnalysis::DyckModRefAnalysis() : ModulePass(ID) {}

DyckModRefAnalysis::~DyckModRefAnalysis() = default;

void DyckModRefAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<DyckAliasAnalysis>();
}

bool DyckModRefAnalysis::runOnModule(Module &M) {
  RecursiveTimer DyckMRA("Running DyckMRA");
  auto *DyckAA = &getAnalysis<DyckAliasAnalysis>();
  MRAnalyzer MR(&M, DyckAA->getDyckGraph(), DyckAA->getDyckCallGraph());
  MR.intraProcedureAnalysis();
  MR.interProcedureAnalysis();
  MR.swap(Func2MR); // get the result
  return false;
}

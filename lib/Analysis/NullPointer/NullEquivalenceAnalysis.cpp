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

#include "Analysis/NullPointer/NullEquivalenceAnalysis.h"

#include <llvm/IR/Instructions.h>

NullEquivalenceAnalysis::NullEquivalenceAnalysis(Function *F) {
  // init
  for (unsigned K = 0; K < F->arg_size(); ++K) {
    auto *Arg = F->getArg(K);
    DisSet.makeSet(Arg);
  }
  for (auto &B : *F) {
    for (auto &I : B) {
      for (unsigned K = 0; K < I.getNumOperands(); ++K) {
        auto *Op = I.getOperand(K);
        DisSet.makeSet(Op);
      }
      DisSet.makeSet(&I);
    }
  }

  // merge
  for (auto &B : *F) {
    for (auto &I : B) {
      if (auto *Cast = dyn_cast<CastInst>(&I)) {
        DisSet.doUnion(&I, Cast->getOperand(0));
      } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        DisSet.doUnion(&I, GEP->getPointerOperand());
      } else if (isa<PHINode>(&I) && I.getNumOperands() == 1) {
        DisSet.doUnion(&I, I.getOperand(0));
      }
    }
  }
}

Value *NullEquivalenceAnalysis::get(Value *V) { return DisSet.findSet(V); }

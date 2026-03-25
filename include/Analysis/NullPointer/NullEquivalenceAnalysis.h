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

#ifndef NULLPOINTER_NULLEQUIVALENCEANALYSIS_H
#define NULLPOINTER_NULLEQUIVALENCEANALYSIS_H

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/Pass.h>
// #include <set>
// #include <unordered_map>

#include "Utils/ADT/DisjointSet.h"

using namespace llvm;

/// a ptr in a group is nonnull, then all ptrs in the group are nonnull
class NullEquivalenceAnalysis {
private:
  DisjointSet<Value *> DisSet;

public:
  explicit NullEquivalenceAnalysis(Function *);

  Value *get(Value *);
};

#endif // NULLPOINTER_NULLEQUIVALENCEANALYSIS_H

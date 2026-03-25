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

#ifndef DYCKAA_DYCKMODREFANALYSIS_H
#define DYCKAA_DYCKMODREFANALYSIS_H

#include "Alias/DyckAA/DyckGraphNode.h"

#include <set>

#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

typedef struct ModRef {
  std::set<DyckGraphNode *> Mods;
  std::set<DyckGraphNode *> Refs;
} ModRef;

class DyckModRefAnalysis : public ModulePass {
private:
  std::map<Function *, ModRef> Func2MR;

public:
  static char ID;

  DyckModRefAnalysis();

  ~DyckModRefAnalysis() override;

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

public:
  std::set<DyckGraphNode *>::iterator mod_begin(Function *F) {
    return Func2MR.at(F).Mods.begin();
  }

  std::set<DyckGraphNode *>::iterator mod_end(Function *F) {
    return Func2MR.at(F).Mods.end();
  }

  std::set<DyckGraphNode *>::iterator ref_begin(Function *F) {
    return Func2MR.at(F).Refs.begin();
  }

  std::set<DyckGraphNode *>::iterator ref_end(Function *F) {
    return Func2MR.at(F).Refs.end();
  }

  bool count(Function *F) const { return Func2MR.count(F); }
};

#endif // DYCKAA_DYCKMODREFANALYSIS_H

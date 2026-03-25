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

#include "Analysis/NullPointer/NullCheckAnalysis.h"

#include "Analysis/NullPointer/LocalNullCheckAnalysis.h"
#include "Analysis/NullPointer/NullFlowAnalysis.h"
#include "Utils/LLVM/RecursiveTimer.h"
#include "Utils/Parallel/ThreadPool.h"

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<unsigned> Round("nca-round", cl::init(2), cl::Hidden,
                               cl::desc("# rounds"));
// Add option to control per-function statistics
static cl::opt<bool> PrintPerFunction(
    "print-per-function",
    cl::desc("Print per-function statistics for context-insensitive analysis"),
    cl::init(false));

char NullCheckAnalysis::ID = 0;
static RegisterPass<NullCheckAnalysis>
    X("nca", "soundly checking if a pointer may be nullptr.");

NullCheckAnalysis::~NullCheckAnalysis() {
  std::lock_guard<std::mutex> lock(AnalysisMapMutex);
  for (auto &It : AnalysisMap)
    delete It.second;
  decltype(AnalysisMap)().swap(AnalysisMap);
}

void NullCheckAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<NullFlowAnalysis>();
}

bool NullCheckAnalysis::runOnModule(Module &M) {
  // record time
  RecursiveTimer TR("Running NullCheckAnalysis");

  // get the null flow analysis
  auto *NFA = &getAnalysis<NullFlowAnalysis>();

  // allocate space for each function for thread safety
  std::set<Function *> Funcs;
  std::vector<Function *> OrderedFuncs;
  for (auto &F : M)
    if (!F.empty()) {
      AnalysisMap[&F] = nullptr;
      Funcs.insert(&F);
      OrderedFuncs.push_back(&F);
    }

  unsigned Count = 1;
  do {
    RecursiveTimer Iteration("NCA Iteration " + std::to_string(Count));
    ThreadPool::get()->parallelForEach(OrderedFuncs, 1,
                                       [this, NFA, &Funcs](Function *F) {
                                         if (!Funcs.count(F))
                                           return;

                                         LocalNullCheckAnalysis *LNCA = nullptr;
                                         {
                                           std::lock_guard<std::mutex> lock(
                                               AnalysisMapMutex);
                                           auto it = AnalysisMap.find(F);
                                           if (it != AnalysisMap.end())
                                             LNCA = it->second;
                                           if (!LNCA) {
                                             LNCA = new LocalNullCheckAnalysis(
                                                 NFA, F);
                                             AnalysisMap[F] = LNCA;
                                           }
                                         }
                                         LNCA->run();
                                       });
    Funcs.clear();
  } while (Count++ < Round.getValue() && NFA->recompute(Funcs));

  // Collect and print statistics
  unsigned TotalPtrInsts = 0;
  unsigned NotNullPtrInsts = 0;
  std::map<Function *, std::pair<unsigned, unsigned>> FunctionStats;

  for (auto &F : M) {
    if (F.empty())
      continue;

    unsigned FuncTotalPtrs = 0;
    unsigned FuncNotNullPtrs = 0;

    for (auto &BB : F) {
      for (auto &I : BB) {
        // Count pointer operands
        for (unsigned i = 0; i < I.getNumOperands(); i++) {
          Value *Op = I.getOperand(i);
          if (Op->getType()->isPointerTy()) {
            TotalPtrInsts++;
            FuncTotalPtrs++;
            // Check if this operand is proven not-null
            // Only check if the operand is a valid operand of the instruction
            if (!mayNull(Op, &I)) {
              NotNullPtrInsts++;
              FuncNotNullPtrs++;
            }
          }
        }
      }
    }

    FunctionStats[&F] = {FuncTotalPtrs, FuncNotNullPtrs};
  }

  errs() << "\n=== Context-Insensitive Analysis Statistics ===\n";
  errs() << "Total pointer operands: " << TotalPtrInsts << "\n";
  errs() << "Pointer operands proven NOT_NULL: " << NotNullPtrInsts << "\n";
  errs() << "Percentage of NOT_NULL pointers: "
         << (TotalPtrInsts > 0 ? (NotNullPtrInsts * 100.0 / TotalPtrInsts) : 0)
         << "%\n";

  // Only print per-function statistics if enabled
  if (PrintPerFunction) {
    errs() << "\nPer-function statistics:\n";
    for (auto &Stat : FunctionStats) {
      Function *F = Stat.first;
      unsigned FuncTotal = Stat.second.first;
      unsigned FuncNotNull = Stat.second.second;

      if (FuncTotal > 0) {
        errs() << "  " << F->getName() << ": " << FuncNotNull << "/"
               << FuncTotal << " NOT_NULL pointers ("
               << (FuncNotNull * 100.0 / FuncTotal) << "%)\n";
      }
    }
  }
  errs() << "================================================\n\n";

  return false;
}

bool NullCheckAnalysis::mayNull(Value *Ptr, Instruction *Inst) {
  auto It = AnalysisMap.find(Inst->getFunction());
  if (It != AnalysisMap.end())
    return It->second->mayNull(Ptr, Inst);
  else
    return true;
}

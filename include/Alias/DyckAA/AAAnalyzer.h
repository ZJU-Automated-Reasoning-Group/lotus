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

#ifndef DYCKAA_AAANALYZER_H
#define DYCKAA_AAANALYZER_H

#include "Alias/DyckAA/DyckCallGraph.h"
#include "Alias/DyckAA/DyckGraph.h"
#include "Alias/Spec/AliasSpecManager.h"

#include <map>
#include <set>
#include <unordered_map>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/Pass.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

typedef struct FunctionTypeNode {
  FunctionType *FuncTy;
  FunctionTypeNode *Root;
  std::set<Function *> CompatibleFuncs;
} FunctionTypeNode;

class AAAnalyzer {
private:
  Module *Mod;
  const DataLayout *DL;
  DyckGraph *CFLGraph;
  DyckCallGraph *DyckCG;

  /// For checking compatible functions of a function pointer
  /// @{
  std::map<Type *, FunctionTypeNode *> FunctionTyNodeMap;
  std::set<FunctionTypeNode *> TyRoots;
  /// @}

  /// Spec manager for handling library functions
  lotus::alias::AliasSpecManager specManager;

public:
  AAAnalyzer(Module *, DyckGraph *, DyckCallGraph *);

  ~AAAnalyzer();

  void intraProcedureAnalysis();

  void interProcedureAnalysis();

private:
  void printNoAliasedPointerCalls();

  void handleInst(Instruction *Inst, DyckCallGraphNode *Parent);

  void handleInstrinsic(Instruction *Inst);

  void handleExtractInsertValueInst(Value *AggValue, Type *AggTy,
                                    ArrayRef<unsigned> &Indices,
                                    Value *InsertedOrExtractedValue);

  DyckGraphNode *handleGEP(GEPOperator *);

  void handleExtractInsertElmtInst(Value *Vec, Value *Elmt);

  void handleInvokeCallInst(Instruction *Ret, Value *CV,
                            std::vector<Value *> *Args,
                            DyckCallGraphNode *Parent);

  void handleLibInvokeCallInst(Value *Ret, Function *F,
                               const std::vector<Value *> *Args,
                               DyckCallGraphNode *Parent);

  bool handlePointerFunctionCalls(DyckCallGraphNode *Caller, int Counter);

  void handleCommonFunctionCall(Call *, DyckCallGraphNode *Caller,
                                DyckCallGraphNode *Callee);

private:
  int isCompatible(FunctionType *, FunctionType *);

  std::set<Function *> *getCompatibleFunctions(FunctionType *);

  FunctionTypeNode *initFunctionGroup(FunctionType *);

  void initFunctionGroups();

  void destroyFunctionGroups();

  void combineFunctionGroups(FunctionType *, FunctionType *);

private:
  /// return the structure's field vertex
  DyckGraphNode *addField(DyckGraphNode *Val, long FieldIndex,
                          DyckGraphNode *Field);

  /// if one of add and val is null, create and return it
  /// otherwise return the ptr;
  DyckGraphNode *addPtrTo(DyckGraphNode *Address, DyckGraphNode *Val);

  DyckGraphNode *makeAlias(DyckGraphNode *, DyckGraphNode *);

  void makeContentAlias(DyckGraphNode *, DyckGraphNode *);

  DyckGraphNode *wrapValue(Value *);
};

#endif // DYCKAA_AAANALYZER_H

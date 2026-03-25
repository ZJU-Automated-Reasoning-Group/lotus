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

#ifndef DYCKAA_DYCKCALLGRAPHNODE_H
#define DYCKAA_DYCKCALLGRAPHNODE_H

#include "Utils/ADT/MapIterators.h"

#include <map>
#include <set>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/Pass.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

/// The class does not model inline asm and intrinsics
class Call {
public:
  enum CallKind {
    CK_Common,
    CK_Pointer,
  };

protected:
  CallKind Kind;

  /// if it has a return value, this is the return value;
  /// it may be null, because there exists some implicit calls, like those in
  /// pthread_create it may be a callinst or invoke inst, currently only call
  /// inst because all invokes are lowered to call
  Instruction *Inst;

  /// the called value
  Value *CalledValue;

  /// the arguments
  std::vector<Value *> Args;

  /// unique id
  int CallId;

public:
  Call(CallKind K, Instruction *Inst, Value *CalledValue,
       std::vector<Value *> *Args);

  CallKind getKind() const { return Kind; }

  Value *getCalledValue() const { return CalledValue; }

  Instruction *getInstruction() const { return Inst; }

  unsigned numArgs() const { return Args.size(); }

  Value *getArg(unsigned K) const { return Args[K]; }

  const std::vector<Value *> &getArgs() const { return Args; }

  int id() const { return CallId; }
};

class CommonCall : public Call {
public:
  CommonCall(Instruction *Inst, Function *Func, std::vector<Value *> *Args);

  Function *getCalledFunction() const {
    return dyn_cast_or_null<Function>(CalledValue);
  }

public:
  static bool classof(const Call *N) { return N->getKind() == CK_Common; }
};

class PointerCall : public Call {
private:
  std::set<Function *> MayAliasedCallees;

public:
  PointerCall(Instruction *Inst, Value *CalledValue,
              std::vector<Value *> *Args);

  std::set<Function *>::const_iterator begin() const {
    return MayAliasedCallees.begin();
  }

  std::set<Function *>::const_iterator end() const {
    return MayAliasedCallees.end();
  }

  bool empty() const { return MayAliasedCallees.empty(); }

  unsigned size() const { return MayAliasedCallees.size(); }

  void addMayAliasedFunction(Function *F) { MayAliasedCallees.insert(F); }

public:
  static bool classof(const Call *N) { return N->getKind() == CK_Pointer; }
};

class DyckCallGraphNode;

typedef std::pair<Call *, DyckCallGraphNode *> CallRecordTy;
typedef std::vector<CallRecordTy> CallRecordVecTy;

class DyckCallGraphNode {
private:
  Function *Func;
  std::set<Value *> Rets;

  std::vector<Value *> Args;
  std::vector<Value *> VAArgs;

  /// call instructions in the function
  /// @{
  std::set<CommonCall *> CommonCalls;
  std::set<PointerCall *> PointerCalls;
  std::map<Instruction *, Call *> InstructionCallMap;
  CallRecordVecTy CallRecords;
  /// @}

public:
  explicit DyckCallGraphNode(Function *);

  ~DyckCallGraphNode();

  Function *getLLVMFunction();

  void addCommonCall(CommonCall *);

  void addPointerCall(PointerCall *);

  void addRet(Value *);

  void addArg(Value *);

  void addVAArg(Value *);

  std::vector<Value *> &getArgs();

  std::vector<Value *> &getVAArgs();

  std::set<Value *> &getReturns();

  Call *getCall(Instruction *);

  void addCalledFunction(Call *C, DyckCallGraphNode *N) {
    CallRecords.emplace_back(C, N);
  }

  std::set<CommonCall *>::const_iterator common_call_begin() const {
    return CommonCalls.begin();
  }

  std::set<CommonCall *>::const_iterator common_call_end() const {
    return CommonCalls.end();
  }

  unsigned common_call_size() const { return CommonCalls.size(); }

  std::set<PointerCall *>::const_iterator pointer_call_begin() const {
    return PointerCalls.begin();
  }

  std::set<PointerCall *>::const_iterator pointer_call_end() const {
    return PointerCalls.end();
  }

  unsigned pointer_call_size() const { return PointerCalls.size(); }

  pair_value_iterator<CallRecordVecTy::iterator, DyckCallGraphNode *>
  child_begin() {
    return {CallRecords.begin()};
  }

  pair_value_iterator<CallRecordVecTy::iterator, DyckCallGraphNode *>
  child_end() {
    return {CallRecords.end()};
  }

  pair_value_iterator<CallRecordVecTy::const_iterator, DyckCallGraphNode *>
  child_begin() const {
    return {CallRecords.begin()};
  }

  pair_value_iterator<CallRecordVecTy::const_iterator, DyckCallGraphNode *>
  child_end() const {
    return {CallRecords.end()};
  }

  CallRecordVecTy::iterator child_edge_begin() { return CallRecords.begin(); }

  CallRecordVecTy::iterator child_edge_end() { return CallRecords.end(); }

  CallRecordVecTy::const_iterator child_edge_begin() const {
    return CallRecords.begin();
  }

  CallRecordVecTy::const_iterator child_edge_end() const {
    return CallRecords.end();
  }
};

#endif // DYCKAA_DYCKCALLGRAPHNODE_H

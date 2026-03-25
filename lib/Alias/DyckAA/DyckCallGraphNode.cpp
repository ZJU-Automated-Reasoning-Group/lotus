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

#include "Alias/DyckAA/DyckCallGraphNode.h"

static int GlobalCallID = 0;

Call::Call(CallKind K, Instruction *Inst, Value *CalledValue,
           std::vector<Value *> *Args)
    : Kind(K) {
  assert(CalledValue != nullptr &&
         "Error when create a call: called value is null!");
  assert(Args != nullptr && "Error when create a call: args is null!");

  this->CalledValue = CalledValue;
  this->Inst = Inst;
  this->Args = *Args;
  this->CallId = ++GlobalCallID;
}

CommonCall::CommonCall(Instruction *Inst, Function *Func,
                       std::vector<Value *> *Args)
    : Call(CK_Common, Inst, Func, Args) {}

PointerCall::PointerCall(Instruction *Inst, Value *CalledValue,
                         std::vector<Value *> *Args)
    : Call(CK_Pointer, Inst, CalledValue, Args) {}

DyckCallGraphNode::DyckCallGraphNode(Function *F) : Func(F) {
  if (!F)
    return;
  for (unsigned K = 0; K < F->arg_size(); ++K)
    Args.push_back(F->getArg(K));
}

DyckCallGraphNode::~DyckCallGraphNode() {
  auto It = PointerCalls.begin();
  while (It != PointerCalls.end()) {
    delete (*It);
    It++;
  }

  auto CIt = CommonCalls.begin();
  while (CIt != CommonCalls.end()) {
    delete *CIt;
    CIt++;
  }
}

void DyckCallGraphNode::addPointerCall(PointerCall *PC) {
  InstructionCallMap.insert(
      std::pair<Instruction *, Call *>(PC->getInstruction(), PC));
  PointerCalls.insert(PC);
}

Function *DyckCallGraphNode::getLLVMFunction() { return Func; }

void DyckCallGraphNode::addCommonCall(CommonCall *CC) {
  InstructionCallMap.insert(
      std::pair<Instruction *, Call *>(CC->getInstruction(), CC));
  CommonCalls.insert(CC);
}

void DyckCallGraphNode::addRet(Value *Ret) { Rets.insert(Ret); }

void DyckCallGraphNode::addArg(Value *Arg) { Args.push_back(Arg); }

void DyckCallGraphNode::addVAArg(Value *Arg) { VAArgs.push_back(Arg); }

std::vector<Value *> &DyckCallGraphNode::getArgs() { return Args; }

std::vector<Value *> &DyckCallGraphNode::getVAArgs() { return VAArgs; }

std::set<Value *> &DyckCallGraphNode::getReturns() { return Rets; }

Call *DyckCallGraphNode::getCall(Instruction *Inst) {
  auto It = InstructionCallMap.find(Inst);
  if (It != InstructionCallMap.end())
    return It->second;
  return nullptr;
}

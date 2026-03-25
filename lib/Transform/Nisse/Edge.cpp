//===-- Edge.cpp --------------------------------------------------===//
// Copyright (C) 2023 Leon Frenot
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the implementation of the Edge
///
//===----------------------------------------------------------------------===//

#include "Transform/Nisse/Nisse.h"

#include <iostream>

using namespace llvm;
using namespace std;

namespace nisse {

BlockPtr Edge::getOrigin() const { return this->origin; }

BlockPtr Edge::getDest() const { return this->dest; }

void Edge::setSESE(llvm::Value *indVar, llvm::Value *initValue,
                   const llvm::APInt *incrValue,
                   llvm::SmallVector<BlockPtr> &exitBlocks, int weight) {
  auto incr = incrValue->signedRoundToDouble();
  if (this->flagSESE &&
      (this->incrValue == 1 || abs(this->incrValue) < abs(incr)))
    return;
  this->indVar = indVar;
  this->initValue = initValue;
  this->incrValue = incr;
  this->exitBlocks = exitBlocks;
  this->weight = weight;
  this->flagSESE = true;
}

bool Edge::isSESE() { return this->flagSESE; }

Instruction *Edge::getInstrumentationPoint() const {
  Instruction *instr;
  if (this->origin->getUniqueSuccessor() == this->dest) {
    instr = this->origin->getTerminator();
  } else {
    instr = &*this->dest->getFirstInsertionPt();
  }

  auto *parent = instr->getParent();
  if (parent == &parent->getParent()->getEntryBlock()) {
    instr = parent->getTerminator();
  }
  return instr;
}

void Edge::insertSimpleIncrFn(int i, Value *inst) {
  auto *instruction = this->getInstrumentationPoint();
  IRBuilder<> builder(instruction);
  auto *L = builder.getInt64Ty();
  Value *indexList[] = {builder.getInt64(i)};
  Value *incr = builder.getInt64(1);
  auto *inst1 = builder.CreateGEP(L, inst, indexList);
  auto *inst2 = builder.CreateLoad(L, inst1);
  auto *inst3 = builder.CreateAdd(inst2, incr);
  builder.CreateStore(inst3, inst1);
}

Value *Edge::createInt32Cast(llvm::Value *inst, IRBuilder<> &builder) {
  Type *int32Ty = builder.getInt32Ty();
  auto *ty = inst->getType();
  if (ty == int32Ty)
    return inst;
  if (ty->isIntegerTy()) {
    return builder.CreateIntCast(inst, int32Ty, true);
  }
  if (ty->isPointerTy()) {
    return builder.CreatePtrToInt(inst, int32Ty);
  }
  return inst;
}

Value *Edge::createInt64Cast(llvm::Value *inst, IRBuilder<> &builder) {
  Type *int64Ty = builder.getInt64Ty();
  auto *ty = inst->getType();
  if (ty == int64Ty)
    return inst;
  if (ty->isIntegerTy()) {
    return builder.CreateIntCast(inst, int64Ty, true);
  }
  if (ty->isPointerTy()) {
    return builder.CreatePtrToInt(inst, int64Ty);
  }
  return inst;
}

void Edge::insertSESEIncrFn(int i, Value *inst) {
  for (auto *block : this->exitBlocks) {
    Instruction *instruction = &*block->getFirstInsertionPt();
    IRBuilder<> builder(instruction);
    Type *int64Ty = builder.getInt64Ty();
    Value *indexList[] = {builder.getInt64(i)};

    auto *incrValueCst = builder.getInt64(this->incrValue);

    auto *inst1 = builder.CreateGEP(int64Ty, inst, indexList);
    auto *inst = builder.CreateLoad(int64Ty, inst1);
    auto *indVarCast = this->createInt64Cast(indVar, builder);
    auto *initValueCast = this->createInt64Cast(initValue, builder);
    Value *incr;
    switch ((int)incrValue) {
    case 1:
      incr = builder.CreateSub(indVarCast, initValueCast);
      break;

    case -1:
      incr = builder.CreateSub(initValueCast, indVarCast);
      break;

    default:
      auto *inst3 = builder.CreateSub(indVarCast, initValueCast);
      auto *incr0 = builder.CreateSDiv(inst3, incrValueCst);
      incr = builder.CreateIntCast(incr0, builder.getInt64Ty(), true);
      break;
    }

    auto *inst4 = builder.CreateAdd(inst, incr);
    builder.CreateStore(inst4, inst1);
  }
}

void Edge::insertIncrFn(int i, Value *inst) {
  if (this->flagSESE) {
    this->insertSESEIncrFn(i, inst);
  } else {
    this->insertSimpleIncrFn(i, inst);
  }
}

int Edge::getIndex() const { return this->index; }

string Edge::getName() const { return to_string(this->index); }

bool Edge::operator==(const Edge &e) const {
  return this->origin == e.origin && this->dest == e.dest;
}

bool Edge::operator<(const Edge &e) const {
  if (this->weight != e.weight)
    return this->weight < e.weight;
  return this->index < e.index;
}

bool Edge::operator>(const Edge &e) const {
  if (this->weight != e.weight)
    return this->weight > e.weight;
  return this->index > e.index;
}

bool Edge::compareWeights(const Edge &a, const Edge &b) { return b < a; }

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const Edge &e) {
  os << e.getName() << " : " << e.getOrigin()->getName() << " -> "
     << e.getDest()->getName();
  return os;
}

std::ostream &operator<<(std::ostream &os, const Edge &e) {
  os << to_string(e.getIndex()) << string(" ")
     << AnalysisUtil::removebb(e.getOrigin()->getName().str()) << string(" ")
     << AnalysisUtil::removebb(e.getDest()->getName().str());
  return os;
}

} // namespace nisse
//===-- NissePass.cpp --------------------------------------------------===//
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
/// This file contains the implementation of the NissePass
///
//===----------------------------------------------------------------------===//

#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/CommandLine.h"

#include "Transform/Nisse/Nisse.h"

#include <iostream>

static llvm::cl::opt<bool>
    DisableProfilePrinting("nisse-disable-print", llvm::cl::init(false),
                           llvm::cl::desc("Disable Profile Printing"));

using namespace llvm;
using namespace std;

namespace nisse {

std::pair<llvm::Value *, llvm::Value *>
NissePass::insertEntryFn(Function &F, multiset<Edge> &reverseSTEdges) {
  int size = reverseSTEdges.size();
  IRBuilder<> builder(&F.getEntryBlock(), F.getEntryBlock().begin());
  auto *I = builder.getInt32Ty();
  ArrayType *arrayType = ArrayType::get(I, size);
  Value *indexList[] = {builder.getInt32(0)};
  auto *zero = builder.getInt8(0);

  auto *counterInst = builder.CreateAlloca(arrayType, nullptr, "counter-array");
  auto *cast = builder.CreateGEP(I, counterInst, indexList);
  builder.CreateMemSet(cast, zero, size * 4, counterInst->getAlign());

  auto *indexInst = builder.CreateAlloca(arrayType, nullptr, "index-array");
  int index = 0;
  for (auto e : reverseSTEdges) {
    auto *indexCst = builder.getInt32(e.getIndex());
    Value *indexList[] = {builder.getInt32(index++)};

    auto *cast = builder.CreateGEP(I, indexInst, indexList);
    builder.CreateStore(indexCst, cast);
  }

  return std::pair<llvm::Value *, llvm::Value *>(counterInst, indexInst);
}

void NissePass::insertExitFn(llvm::Module &M, llvm::Function &F,
                             llvm::Value *counterInst, llvm::Value *indexInst,
                             int size) {
  LLVMContext &Ctx = M.getContext();

  Type *r_type = Type::getVoidTy(Ctx);

  SmallVector<Type *, 3> a_types;
  SmallVector<Value *, 3> a_vals;

  a_types.push_back(CounterArray->getType());
  a_vals.push_back(CounterArray);

  a_types.push_back(IndexArray->getType());
  a_vals.push_back(IndexArray);

  a_types.push_back(Type::getInt32Ty(Ctx));
  a_vals.push_back(ConstantInt::get(Type::getInt32Ty(Ctx), size));

  FunctionType *f_type = FunctionType::get(r_type, a_types, false);
  FunctionCallee f_call =
      M.getOrInsertFunction("nisse_pass_print_data", f_type);

  for (BasicBlock &BB : F) {
    Instruction *Terminator = BB.getTerminator();
    if (isa<ReturnInst>(Terminator)) {
      IRBuilder<> builder(Terminator);
      builder.CreateCall(f_call, a_vals);
    }
  }
}

PreservedAnalyses NissePass::run(Module &M, ModuleAnalysisManager &MAM) {
  LLVMContext &Ctx = M.getContext();
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  outfile.open("info.prof");
  // Associate function to its number of edges
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto &edges = FAM.getResult<NisseAnalysis>(F);
    auto &reverseSTEdges = get<2>(edges);
    int size = reverseSTEdges.size();

    if (size == 1) {
      errs() << "Function '" << F.getName()
             << "' has only 1 edge to instrument. Skipping...\n";
      continue;
    }

    NumEdges += size;
    FunctionSize[F.getName().str()] = size;
    outfile << F.getName().str() << " " << size << "\n";
  }
  outfile.close();

  // Initialize global variables
  ArrayType *CounterArrayType = ArrayType::get(Type::getInt64Ty(Ctx), NumEdges);
  CounterArray = new GlobalVariable(
      M, CounterArrayType, false, GlobalValue::ExternalLinkage,
      Constant::getNullValue(CounterArrayType), "counter-array");

  ArrayType *IndexArrayType = ArrayType::get(Type::getInt32Ty(Ctx), NumEdges);
  IndexArray =
      new GlobalVariable(M, IndexArrayType, false, GlobalValue::ExternalLinkage,
                         Constant::getNullValue(IndexArrayType), "index-array");

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto &edges = FAM.getResult<NisseAnalysis>(F);
    auto &reverseSTEdges = get<2>(edges);
    int size = reverseSTEdges.size();

    // if (F.getName() == "main") {
    //   IRBuilder<> mainBuilder(&F.getEntryBlock(), F.getEntryBlock().begin());
    //   auto cast = mainBuilder.CreateGEP(mainBuilder.getInt64Ty(),
    //   CounterArray, {mainBuilder.getInt64(0)}); auto zero =
    //   mainBuilder.getInt64(0); mainBuilder.CreateMemSet(cast, zero, NumEdges
    //   * sizeof(int64_t), CounterArray->getAlign());
    // }

    if (size == 1) {
      if (!DisableProfilePrinting)
        if (F.getName() == "main")
          this->insertExitFn(M, F, CounterArray, IndexArray, NumEdges);
      continue;
    }

    // Equivalent to InsertEntryFn for IndexArray
    IRBuilder<> builder(&F.getEntryBlock(), F.getEntryBlock().begin());

    Type *Int32Ty = builder.getInt32Ty();

    int index = Offset;
    for (auto e : reverseSTEdges) {
      auto *indexCst = builder.getInt32(e.getIndex());
      Value *indexList[] = {builder.getInt32(index++)};

      auto *cast = builder.CreateGEP(Int32Ty, IndexArray, indexList);
      builder.CreateStore(indexCst, cast);
    }

    index = Offset;
    for (auto p : reverseSTEdges) {
      p.insertIncrFn(index++, CounterArray);
    }

    if (!DisableProfilePrinting)
      if (F.getName() == "main")
        this->insertExitFn(M, F, CounterArray, IndexArray, NumEdges);

    Offset += size;
  }

  return PreservedAnalyses::all();
}

PreservedAnalyses KSPass::run(Module &M, ModuleAnalysisManager &MAM) {
  LLVMContext &Ctx = M.getContext();
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  outfile.open("info.prof");
  // Associate function to its number of edges
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto &edges = FAM.getResult<KSAnalysis>(F);
    auto &reverseSTEdges = get<2>(edges);
    int size = reverseSTEdges.size();

    if (size == 1) {
      errs() << "Function '" << F.getName()
             << "' has only 1 edge to instrument. Skipping...\n";
      continue;
    }

    NumEdges += size;
    FunctionSize[F.getName().str()] = size;
    outfile << F.getName().str() << " " << size << "\n";
  }
  outfile.close();

  // Initialize global variables
  ArrayType *CounterArrayType = ArrayType::get(Type::getInt64Ty(Ctx), NumEdges);
  CounterArray = new GlobalVariable(
      M, CounterArrayType, false, GlobalValue::ExternalLinkage,
      Constant::getNullValue(CounterArrayType), "counter-array");

  ArrayType *IndexArrayType = ArrayType::get(Type::getInt32Ty(Ctx), NumEdges);
  IndexArray =
      new GlobalVariable(M, IndexArrayType, false, GlobalValue::ExternalLinkage,
                         Constant::getNullValue(IndexArrayType), "index-array");

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto &edges = FAM.getResult<KSAnalysis>(F);
    auto &reverseSTEdges = get<2>(edges);
    int size = reverseSTEdges.size();

    if (size == 1) {
      if (!DisableProfilePrinting)
        if (F.getName() == "main")
          this->insertExitFn(M, F, CounterArray, IndexArray, NumEdges);
      continue;
    }

    // Equivalent to InsertEntryFn for IndexArray
    IRBuilder<> builder(&F.getEntryBlock(), F.getEntryBlock().begin());

    Type *Int32Ty = builder.getInt32Ty();

    int index = Offset;
    for (auto e : reverseSTEdges) {
      auto *indexCst = builder.getInt32(e.getIndex());
      Value *indexList[] = {builder.getInt32(index++)};

      auto *cast = builder.CreateGEP(Int32Ty, IndexArray, indexList);
      builder.CreateStore(indexCst, cast);
    }

    index = Offset;
    for (auto p : reverseSTEdges) {
      p.insertIncrFn(index++, CounterArray);
    }

    if (!DisableProfilePrinting)
      if (F.getName() == "main")
        this->insertExitFn(M, F, CounterArray, IndexArray, NumEdges);

    Offset += size;
  }

  return PreservedAnalyses::all();
}

} // namespace nisse

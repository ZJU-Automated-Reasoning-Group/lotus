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

#include "Alias/DyckAA/AAAnalyzer.h"

#include "Utils/LLVM/RecursiveTimer.h"

#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InstIterator.h>

static cl::opt<unsigned> FunctionTypeCheckLevel(
    "function-type-check-level", cl::init(4), cl::Hidden,
    cl::desc("The level of checking the compatability of function types"
             "4: equivalent function types"
             "3: same # parameters and same store size of each param"
             "2: same # parameters and consider only ptr/int type of params"
             "1: same # parameters"
             "0: completely not compatible function types"));

static cl::opt<bool> WithFunctionCastComb(
    "with-function-cast-comb", cl::init(false), cl::Hidden,
    cl::desc("Two func types are compatible if there's a cast between them."));

static cl::opt<bool>
    PrintUnknownPointerCall("print-unknown-ptr-call", cl::init(false),
                            cl::Hidden, cl::desc("print unknown ptr call"));

static cl::opt<unsigned> NumInterIteration(
    "dyckaa-inter-iteration", cl::init(UINT_MAX), cl::Hidden,
    cl::desc("The max # iterators for fixed-point inter-proc computation."));

AAAnalyzer::AAAnalyzer(Module *M, DyckGraph *DG, DyckCallGraph *CG) {
  Mod = M;
  CFLGraph = DG;
  DyckCG = CG;
  DL = &M->getDataLayout();
  specManager.initialize(*M);
  initFunctionGroups();
}

AAAnalyzer::~AAAnalyzer() { destroyFunctionGroups(); }

void AAAnalyzer::intraProcedureAnalysis() {
  RecursiveTimer IntraAA("Running intra-procedural analysis");
  long InstNum = 0;
  long IntrinsicsNum = 0;
  for (auto &F : *Mod) {
    if (F.isIntrinsic()) {
      // intrinsics are handled as instructions
      IntrinsicsNum++;
      continue;
    }
    DyckCallGraphNode *DF = DyckCG->getOrInsertFunction(&F);
    for (auto &I : instructions(F)) {
      InstNum++;
      handleInst(&I, DF);
    }
  }
  DEBUG_WITH_TYPE("dyckaa-stats",
                  errs() << "\n# Instructions: " << InstNum << "\n");
  DEBUG_WITH_TYPE("dyckaa-stats",
                  errs() << "# Functions: " << Mod->size() - IntrinsicsNum
                         << "\n");
}

void AAAnalyzer::interProcedureAnalysis() {
  RecursiveTimer IntraAA("Running inter-procedural analysis");

  unsigned IterationCounter = 0;
  while (true) {
    if (IterationCounter++ >= NumInterIteration.getValue())
      break;
    RecursiveTimer IterationTimer("Iteration " +
                                  std::to_string(IterationCounter));

    bool Finished = true;
    CFLGraph->qirunAlgorithm();

    if (IterationCounter == 1) { // direct calls
      RecursiveTimer DirectCallTimer("Handling direct calls");
      auto CGNodeIt = DyckCG->nodes_begin();
      while (CGNodeIt != DyckCG->nodes_end()) {
        DyckCallGraphNode *CGNode = *CGNodeIt;
        auto CIt = CGNode->common_call_begin();
        while (CIt != CGNode->common_call_end()) {
          Finished = false;
          CommonCall *CC = *CIt;
          Function *CV = CC->getCalledFunction();
          assert(CV && "Error: it is not a function in common calls!");
          handleCommonFunctionCall(CC, CGNode, DyckCG->getOrInsertFunction(CV));
          ++CIt;
        }
        ++CGNodeIt;
      }
    }

    { // indirect call
      RecursiveTimer DirectCallTimer("Handling indirect calls");
      int NumProcessedFunctions = 0;
      auto DFIt = DyckCG->begin();
      while (DFIt != DyckCG->end()) {
        DyckCallGraphNode *DF = DFIt->second;

        if (handlePointerFunctionCalls(DF, ++NumProcessedFunctions)) {
          Finished = false;
        }

        ++DFIt;
      }
    }

    if (Finished)
      break;
  }

  // finalize the call graph
  for (auto &F : *Mod) {
    auto *FN = DyckCG->getOrInsertFunction(&F);
    for (auto It = FN->common_call_begin(), E = FN->common_call_end(); It != E;
         ++It) {
      auto *CC = *It;
      auto *Callee = CC->getCalledFunction();
      auto *CalleeN = DyckCG->getOrInsertFunction(Callee);
      FN->addCalledFunction(CC, CalleeN);
    }

    for (auto It = FN->pointer_call_begin(), E = FN->pointer_call_end();
         It != E; ++It) {
      auto *PC = *It;
      for (auto *Callee : *PC) {
        auto *CalleeN = DyckCG->getOrInsertFunction(Callee);
        FN->addCalledFunction(PC, CalleeN);
      }
    }
  }

  if (PrintUnknownPointerCall)
    printNoAliasedPointerCalls();
}

void AAAnalyzer::printNoAliasedPointerCalls() {
  unsigned Size = 0;

  outs() << ">>>>>>>>>> Pointer calls that do not find any aliased function\n";
  auto DFIt = DyckCG->begin();
  while (DFIt != DyckCG->end()) {
    DyckCallGraphNode *DF = DFIt->second;
    auto PCIt = DF->pointer_call_begin();
    while (PCIt != DF->pointer_call_end()) {
      PointerCall *PC = *PCIt;
      if (PC->empty()) {
        Size++;
        if (PC->getInstruction()) {
          outs() << *(PC->getInstruction()) << "\n";
        } else {
          outs() << "Implicit calls in " << DF->getLLVMFunction()->getName()
                 << "\n";
        }
      }

      PCIt++;
    }

    DFIt++;
  }
  outs() << "<<<<<<<<<< Total: " << Size << ".\n\n";
}

//// The followings are private functions

int AAAnalyzer::isCompatible(FunctionType *X, FunctionType *Y) {
  if (X == Y)
    return 4;

  if (X->isVarArg() != Y->isVarArg())
    return 0;

  Type *XRet = X->getReturnType();
  Type *YRet = Y->getReturnType();

  if (XRet->isVoidTy() != YRet->isVoidTy())
    return 0;

  unsigned XNumParam = X->getNumParams();
  unsigned YNumParam = Y->getNumParams();
  if (XNumParam == YNumParam) {
    bool Level3 = true;
    for (unsigned K = 0; K < XNumParam; K++) {
      if (DL->getTypeStoreSize(X->getParamType(K)) !=
          DL->getTypeStoreSize(Y->getParamType(K))) {
        Level3 = false;
        break;
      }
    }

    if (Level3)
      return 3;

    bool Level2 = true;
    for (unsigned K = 0; K < XNumParam; K++) {
      Type *XParamKTy = X->getParamType(K);
      Type *YParamKTy = Y->getParamType(K);

      if (XParamKTy->isIntegerTy() != YParamKTy->isIntegerTy()) {
        Level2 = false;
        break;
      }

      if (XParamKTy->isPointerTy() != YParamKTy->isPointerTy()) {
        Level2 = false;
        break;
      }
    }

    if (Level2)
      return 2;

    return 1;
  } else {
    return 0;
  }
}

FunctionTypeNode *AAAnalyzer::initFunctionGroup(FunctionType *Ty) {
  auto It = FunctionTyNodeMap.find(Ty);
  if (It != FunctionTyNodeMap.end())
    return It->second->Root;

  // iterate all roots to check whether compatible
  // if so, add it to its set
  // otherwise, new a root
  auto RIt = TyRoots.begin();
  while (RIt != TyRoots.end()) {
    if ((unsigned)isCompatible((*RIt)->FuncTy, Ty) ==
        FunctionTypeCheckLevel.getValue()) {
      auto *TyNode = new FunctionTypeNode;
      TyNode->FuncTy = Ty;
      TyNode->Root = (*RIt);

      FunctionTyNodeMap.insert(
          std::pair<Type *, FunctionTypeNode *>(Ty, TyNode));
      return *RIt;
    }
    RIt++;
  }

  // not found compatible ones, create a new one
  auto *TyNode = new FunctionTypeNode;
  TyNode->FuncTy = Ty;
  TyNode->Root = TyNode;

  TyRoots.insert(TyNode);
  FunctionTyNodeMap.insert(std::pair<Type *, FunctionTypeNode *>(Ty, TyNode));
  return TyNode;
}

void AAAnalyzer::initFunctionGroups() {
  for (auto &FunRef : *Mod) {
    Function *Func = &FunRef;
    if (Func->isIntrinsic()) {
      continue;
    }

    bool OnlyUsedAsCallFunc = true;
    for (Value::user_iterator I = Func->user_begin(), E = Func->user_end();
         I != E; ++I) {
      User *U = (User *)(*I);
      if (isa<CallInst>(U) &&
          ((CallInst *)U)->getCalledFunction() == Func) { // all invoke -> call
        continue;
      } else {
        OnlyUsedAsCallFunc = false;
        break;
      }
    }

    // the function only used in call instructions cannot alias with
    // certain function pointers
    if (!OnlyUsedAsCallFunc) {
      auto *FTy = (FunctionType *)((PointerType *)Func->getType())
                      ->getPointerElementType();

      FunctionTypeNode *Root = this->initFunctionGroup(FTy);
      Root->CompatibleFuncs.insert(Func);
    }
  }
}

void AAAnalyzer::destroyFunctionGroups() {
  auto It = FunctionTyNodeMap.begin();
  while (It != FunctionTyNodeMap.end())
    delete It++->second;
  FunctionTyNodeMap.clear();
  TyRoots.clear();
}

void AAAnalyzer::combineFunctionGroups(FunctionType *FTyX, FunctionType *FTyY) {
  if (!WithFunctionCastComb)
    return;

  FunctionTypeNode *X = this->initFunctionGroup(FTyX)->Root;
  FunctionTypeNode *Y = this->initFunctionGroup(FTyY)->Root;

  if (X == Y)
    return;

  DEBUG_WITH_TYPE("combine-function-groups", outs() << "[CANARY] Combining "
                                                    << *FTyX << " and " << *FTyY
                                                    << "... \n");

  X->CompatibleFuncs.insert(Y->CompatibleFuncs.begin(),
                            Y->CompatibleFuncs.end());
  Y->CompatibleFuncs.insert(X->CompatibleFuncs.begin(),
                            X->CompatibleFuncs.end());
}

DyckGraphNode *AAAnalyzer::addField(DyckGraphNode *Val, long FieldIndex,
                                    DyckGraphNode *Field) {
  if (!Field) {
    auto *ValRepSet = Val->getOutVertices(
        (void *)(CFLGraph->getOrInsertIndexEdgeLabel(FieldIndex)));
    if (ValRepSet && !ValRepSet->empty()) {
      Field = *(ValRepSet->begin());
    } else {
      Field = CFLGraph->retrieveDyckVertex(nullptr).first;
      Val->addTarget(Field,
                     (void *)(CFLGraph->getOrInsertIndexEdgeLabel(FieldIndex)));
    }
  } else {
    Val->addTarget(Field,
                   (void *)(CFLGraph->getOrInsertIndexEdgeLabel(FieldIndex)));
  }
  return Field;
}

DyckGraphNode *AAAnalyzer::addPtrTo(DyckGraphNode *Address,
                                    DyckGraphNode *Val) {
  assert((Address || Val) && "ERROR in addPtrTo\n");

  auto *DLabel = CFLGraph->getDereferenceEdgeLabel();
  if (!Address) {
    Address = CFLGraph->retrieveDyckVertex(nullptr).first;
    Address->addTarget(Val, DLabel);
    return Address;
  } else if (!Val) {
    std::set<DyckGraphNode *> *DerefSet = Address->getOutVertices(DLabel);
    if (DerefSet && !DerefSet->empty()) {
      Val = *(DerefSet->begin());
    } else {
      Val = CFLGraph->retrieveDyckVertex(nullptr).first;
      Address->addTarget(Val, DLabel);
    }
    return Val;
  } else {
    Address->addTarget(Val, DLabel);
    return Address;
  }
}

DyckGraphNode *AAAnalyzer::makeAlias(DyckGraphNode *X, DyckGraphNode *Y) {
  // combine x's rep and y's rep
  return CFLGraph->combine(X, Y);
}

void AAAnalyzer::makeContentAlias(DyckGraphNode *X, DyckGraphNode *Y) {
  addPtrTo(Y, addPtrTo(X, nullptr));
}

DyckGraphNode *AAAnalyzer::handleGEP(GEPOperator *GEP) {
  Value *Ptr = GEP->getPointerOperand();
  DyckGraphNode *Current = wrapValue(Ptr);

  auto GTI = gep_type_begin(GEP); // preGTI is the PointerTy of ptr
  Type *AggOrPointerTy = Ptr->getType();

  auto NumIndices = GEP->getNumIndices();
  unsigned IdxIdx = 0;
  while (IdxIdx < NumIndices) {
    Value *Idx = GEP->getOperand(++IdxIdx);
    auto *CI = dyn_cast<ConstantInt>(Idx);
    if (AggOrPointerTy->isStructTy()) {
      // example: gep y 0 constIdx
      // s1: y--deref-->?1--(fieldIdx idxLabel)-->?2
      DyckGraphNode *TheStruct = this->addPtrTo(Current, nullptr);

      assert(CI && "ERROR: when dealing with gep");

      // s2: ?3--deref-->?2
      auto FieldIdx = (unsigned)(*(CI->getValue().getRawData()));
      DyckGraphNode *Field = this->addField(TheStruct, FieldIdx, nullptr);
      DyckGraphNode *FieldPtr = this->addPtrTo(nullptr, Field);

      // the label representation and feature impl is temporal.
      // s3: y--(fieldIdx offLabel)-->?3
      Current->addTarget(
          FieldPtr, (void *)(CFLGraph->getOrInsertOffsetEdgeLabel(FieldIdx)));

      // update current
      Current = FieldPtr;
    } else if (AggOrPointerTy->isPointerTy() || AggOrPointerTy->isArrayTy() ||
               AggOrPointerTy->isVectorTy()) {
      if (!CI)
        wrapValue(Idx);
    } else {
      errs() << "\n";
      errs() << *GEP << "\n";
      errs() << "ERROR in handleGep: unknown type:\n";
      errs() << "Type Id: " << AggOrPointerTy->getTypeID() << "\n";
      errs() << "Type: " << *AggOrPointerTy << "\n";
      exit(1);
    }
    AggOrPointerTy = GTI.getIndexedType();
    GTI++;
  }

  return Current;
}

DyckGraphNode *AAAnalyzer::wrapValue(Value *V) {
  // if the vertex of v exists, return it, otherwise create one
  std::pair<DyckGraphNode *, bool> RetPair = CFLGraph->retrieveDyckVertex(V);
  if (RetPair.second || !V) {
    return RetPair.first;
  }
  DyckGraphNode *VDV = RetPair.first;

  // constantTy are handled as below.
  if (isa<ConstantExpr>(V)) {
    unsigned Opcode = ((ConstantExpr *)V)->getOpcode();
    if (Opcode >= Instruction::CastOpsBegin &&
        Opcode <= Instruction::CastOpsEnd) {
      DyckGraphNode *Got = wrapValue(((ConstantExpr *)V)->getOperand(0));
      VDV = wrapValue(V);
      VDV = makeAlias(VDV, Got);
    } else if (Opcode == Instruction::GetElementPtr) {
      DyckGraphNode *Got = handleGEP((GEPOperator *)V);
      VDV = wrapValue(V);
      VDV = makeAlias(VDV, Got);
    } else if (Opcode == Instruction::Select) {
      wrapValue(((ConstantExpr *)V)->getOperand(0));
      DyckGraphNode *Opt0 = wrapValue(((ConstantExpr *)V)->getOperand(1));
      DyckGraphNode *Opt1 = wrapValue(((ConstantExpr *)V)->getOperand(2));
      VDV = wrapValue(V);
      VDV = makeAlias(VDV, Opt0);
      VDV = makeAlias(VDV, Opt1);
    } else if (Opcode == Instruction::ExtractValue) {
      Value *Agg = ((ConstantExpr *)V)->getOperand(0);
      std::vector<unsigned> IndicesVec;
      for (unsigned K = 1; K < ((ConstantExpr *)V)->getNumOperands(); K++) {
        auto *Index = (ConstantInt *)((ConstantExpr *)V)->getOperand(K);
        IndicesVec.push_back((unsigned)(*(Index->getValue().getRawData())));
      }
      ArrayRef<unsigned> Indices(IndicesVec);
      this->handleExtractInsertValueInst(Agg, Agg->getType(), Indices, V);
    } else if (Opcode == Instruction::InsertValue) {
      DyckGraphNode *ResultV = wrapValue(V);
      Value *Agg = ((ConstantExpr *)V)->getOperand(0);
      if (!isa<UndefValue>(Agg)) {
        makeAlias(ResultV, wrapValue(Agg));
      }
      std::vector<unsigned> IndicesVec;
      for (unsigned K = 2; K < ((ConstantExpr *)V)->getNumOperands(); K++) {
        auto *Index = (ConstantInt *)((ConstantExpr *)V)->getOperand(K);
        IndicesVec.push_back((unsigned)(*(Index->getValue().getRawData())));
      }
      ArrayRef<unsigned> indices(IndicesVec);
      this->handleExtractInsertValueInst(V, Agg->getType(), indices,
                                         ((ConstantExpr *)V)->getOperand(1));
    } else if (Opcode == Instruction::ExtractElement) {
      Value *Vec = ((ConstantExpr *)V)->getOperand(0);
      this->handleExtractInsertElmtInst(Vec, V);
    } else if (Opcode == Instruction::InsertElement) {
      Value *Vec = ((ConstantExpr *)V)->getOperand(0);
      Value *Elmt2Insert = ((ConstantExpr *)V)->getOperand(1);
      this->handleExtractInsertElmtInst(Vec, Elmt2Insert);
      this->handleExtractInsertElmtInst(V, Elmt2Insert);
    } else if (Opcode == Instruction::ShuffleVector) {
      Value *VecX = ((ConstantExpr *)V)->getOperand(0);
      Value *VecY = ((ConstantExpr *)V)->getOperand(1);
      Value *VecRet = V;
      this->makeAlias(wrapValue(VecRet), wrapValue(VecX));
      this->makeAlias(wrapValue(VecRet), wrapValue(VecY));
    } else {
      // binary constant expr
      // cmp constant expr
      // other unary constant expr
      bool BinaryOrUnaryOrCmpConstExpr =
          (Opcode >= Instruction::BinaryOpsBegin &&
           Opcode <= Instruction::BinaryOpsEnd) ||
          (Opcode == Instruction::ICmp || Opcode == Instruction::FCmp) ||
          (((ConstantExpr *)V)->getNumOperands() == 1);
      assert(BinaryOrUnaryOrCmpConstExpr);
      for (unsigned K = 0; K < ((ConstantExpr *)V)->getNumOperands(); K++) {
        // e.g. i1 icmp ne (i8* bitcast (i32 (i32*, void (i8*)*)*
        // @__pthread_key_create to i8*), i8* null) we should handle op<0>
        wrapValue(((ConstantExpr *)V)->getOperand(K));
      }
    }
  } else if (isa<ConstantStruct>(V) || isa<ConstantArray>(V)) {
    auto *Agg = (Constant *)V;
    unsigned NumElmt = Agg->getNumOperands();
    for (unsigned K = 0; K < NumElmt; K++) {
      Value *ElmtK = Agg->getOperand(K);

      std::vector<unsigned> Indices;
      Indices.push_back(K);
      ArrayRef<unsigned> indicesRef(Indices);
      this->handleExtractInsertValueInst(V, Agg->getType(), indicesRef, ElmtK);
    }
    VDV = wrapValue(V);
  } else if (isa<ConstantVector>(V)) {
    auto *CV = (ConstantVector *)V;
    unsigned NumElmt = CV->getNumOperands();
    for (unsigned K = 0; K < NumElmt; K++) {
      Value *ElmtK = CV->getOperand(K);
      this->handleExtractInsertElmtInst(CV, ElmtK);
    }
    VDV = wrapValue(V);
  } else if (isa<GlobalValue>(V)) {
    if (isa<GlobalVariable>(V)) {
      auto *Global = (GlobalVariable *)V;
      if (Global->hasInitializer()) {
        Value *Initializer = Global->getInitializer();
        if (!isa<UndefValue>(Initializer)) {
          DyckGraphNode *InitNode = wrapValue(Initializer);
          VDV = wrapValue(V);
          addPtrTo(VDV, InitNode);
        }
      }
    } else if (isa<GlobalAlias>(V)) {
      auto *Global = (GlobalAlias *)V;
      Value *Aliasee = Global->getAliasee();
      auto *AliaseeV = wrapValue(Aliasee);
      VDV = wrapValue(V);
      VDV = makeAlias(VDV, AliaseeV);
    } else if (isa<Function>(V)) {
      // do nothing
    } else {
      assert(false);
    }
  } else if (isa<ConstantInt>(V) || isa<ConstantFP>(V) ||
             isa<ConstantPointerNull>(V) || isa<UndefValue>(V)) {
    // do nothing
  } else if (isa<ConstantDataArray>(V) || isa<ConstantDataVector>(V)) {
    // ConstantDataSequential

    // ConstantDataVector/Array - A vector/array constant whose element type is
    // a simple 1/2/4/8-byte integer or float/double, and whose elements are
    // just simple data values (i.e. ConstantInt/ConstantFP).  This Constant
    // node has no operands because it stores all the elements of the constant
    // as densely packed data, instead of as Value*'s.

    // e.g.
    //    constant [12 x i8] c"I am happy!\00"
    //    constant <2 x i64> <i64 -1, i64 -1>

    // since the elements are not pointers, we do nothing here.
  } else if (isa<BlockAddress>(V)) {
    // do nothing
  } else if (isa<ConstantAggregateZero>(V)) {
    // do nothing
    // e.g.
    //    [1 x i8] zeroinitializer
    //    %struct.color_cap zeroinitializer
  } else if (isa<Constant>(V)) {
    errs() << "ERROR when handle the following constant value\n";
    errs() << *V << "\n";
    errs().flush();
    exit(-1);
  }

  return VDV;
}

void AAAnalyzer::handleInstrinsic(Instruction *Inst) {
  if (!Inst)
    return;

  int Mask = 0;
  auto *CallI = (IntrinsicInst *)Inst;
  switch (CallI->getIntrinsicID()) {
  // Variable Argument Handling Intrinsics
  case Intrinsic::vastart: {
    Value *VAListPtr = CallI->getArgOperand(0);
    wrapValue(VAListPtr);

    // 0b01
    Mask |= 1;
  } break;
  case Intrinsic::vaend: {
  } break;
  case Intrinsic::vacopy: // the same with memmove/memcpy

    // Standard C Library Intrinsics
  case Intrinsic::memmove:
  case Intrinsic::memcpy: {
    Value *SrcPtr = CallI->getArgOperand(0);
    Value *DstPtr = CallI->getArgOperand(1);

    DyckGraphNode *SrcPtrVer = wrapValue(SrcPtr);
    DyckGraphNode *DstPtrVer = wrapValue(DstPtr);

    DyckGraphNode *SrcVer = addPtrTo(SrcPtrVer, nullptr);
    DyckGraphNode *DstVer = addPtrTo(DstPtrVer, nullptr);

    makeAlias(SrcVer, DstVer);

    // 0b11
    Mask |= 3;
  } break;
  case Intrinsic::memset: {
    Value *Ptr = CallI->getArgOperand(0);
    Value *Val = CallI->getArgOperand(1);
    addPtrTo(wrapValue(Ptr), wrapValue(Val));
    // 0b11
    Mask |= 3;
  } break;
  case Intrinsic::masked_load: {
    Value *VecReturn = CallI;
    Value *VecPassthru = CallI->getArgOperand(2);
    Value *ptr = CallI->getArgOperand(0);

    // semantics:
    // vec_load = load ptr
    // vec_return = select mask vec_load vec_passthru

    this->makeAlias(wrapValue(VecReturn), wrapValue(VecPassthru));
    this->addPtrTo(wrapValue(ptr), wrapValue(VecReturn));

    // 0b101
    Mask |= 5;
  } break;
  case Intrinsic::masked_store: {
    // call void @llvm.masked.store.v16f32(<16 x float> %value, <16 x float>*
    // %ptr, i32 4,  <16 x i1> %mask)

    //;; The result of the following instructions is identical aside from
    //potential data races and memory access exceptions %oldval = load <16 x
    //float>, <16 x float>* %ptr, align 4 %res = select <16 x i1> %mask, <16 x
    //float> %value, <16 x float> %oldval store <16 x float> %res, <16 x float>*
    // %ptr, align 4

    Value *Vec = CallI->getArgOperand(0);
    Value *Ptr = CallI->getArgOperand(1);

    this->addPtrTo(wrapValue(Ptr), wrapValue(Vec));

    // 0b11
    Mask |= 3;
  } break;
    // TODO other C lib intrinsics

    // Accurate Garbage Collection Intrinsics
    // Code Generator Intrinsics
    // Bit Manipulation Intrinsics
    // Exception Handling Intrinsics
    // Trampoline Intrinsics
    // Memory Use Markers
    // General Intrinsics
    // Stack Map Intrinsics

    // Arithmetic with Overflow Intrinsics
    // Specialised Arithmetic Intrinsics
    // Half Precision Floating Point Intrinsics
    // Debugger Intrinsics
  default:
    break;
  }

  // wrap unhandled operand
  for (unsigned K = 0; K < CallI->arg_size(); K++) {
    if (!(Mask & (1 << K))) {
      wrapValue(CallI->getArgOperand(K));
    }
  }
  wrapValue(CallI->getCalledOperand());
}

std::set<Function *> *AAAnalyzer::getCompatibleFunctions(FunctionType *FTy) {
  FunctionTypeNode *FTyNode = this->initFunctionGroup(FTy);
  return &(FTyNode->Root->CompatibleFuncs);
}

void AAAnalyzer::handleInst(Instruction *Inst, DyckCallGraphNode *Parent) {
  int Mask = 0;

  switch (Inst->getOpcode()) {
  // common/bitwise binary operations
  // Terminator instructions
  case Instruction::Ret: {
    ReturnInst *RetInst = ((ReturnInst *)Inst);
    if (RetInst->getNumOperands() > 0 &&
        !RetInst->getOperandUse(0)->getType()->isVoidTy()) {
      Parent->addRet(RetInst->getOperandUse(0));
    }
  } break;
  case Instruction::Switch:
  case Instruction::Br:
  case Instruction::IndirectBr:
  case Instruction::Unreachable:
    break;

    // vector operations
  case Instruction::ExtractElement: {
    Value *Vec = ((ExtractElementInst *)Inst)->getVectorOperand();
    this->handleExtractInsertElmtInst(Vec, Inst);

    Mask |= (~0);
  } break;
  case Instruction::InsertElement: {
    Value *Vec = ((InsertElementInst *)Inst)->getOperand(0);
    Value *Elmt2Insert = ((InsertElementInst *)Inst)->getOperand(1);
    this->handleExtractInsertElmtInst(Vec, Elmt2Insert);
    this->handleExtractInsertElmtInst(Inst, Elmt2Insert);

    Mask |= (~0);
  } break;
  case Instruction::ShuffleVector: {
    Value *Vec1 = ((ShuffleVectorInst *)Inst)->getOperand(0);
    Value *Vec2 = ((ShuffleVectorInst *)Inst)->getOperand(1);
    Value *VectRet = Inst;

    this->makeAlias(wrapValue(VectRet), wrapValue(Vec1));
    this->makeAlias(wrapValue(VectRet), wrapValue(Vec2));

    Mask |= (~0);
  } break;

    // aggregate operations
  case Instruction::ExtractValue: {
    Value *Agg = ((ExtractValueInst *)Inst)->getAggregateOperand();
    ArrayRef<unsigned> Indices = ((ExtractValueInst *)Inst)->getIndices();

    this->handleExtractInsertValueInst(Agg, Agg->getType(), Indices, Inst);

    Mask |= (~0);
  } break;
  case Instruction::InsertValue: {
    DyckGraphNode *ResultV = wrapValue(Inst);
    Value *Agg = ((InsertValueInst *)Inst)->getAggregateOperand();
    if (!isa<UndefValue>(Agg))
      makeAlias(ResultV, wrapValue(Agg));

    ArrayRef<unsigned> Indices = ((InsertValueInst *)Inst)->getIndices();

    this->handleExtractInsertValueInst(
        Inst, Inst->getType(), Indices,
        ((InsertValueInst *)Inst)->getInsertedValueOperand());

    Mask |= (~0);
  } break;

    // memory accessing and addressing operations
  case Instruction::Alloca:
    break;
  case Instruction::Fence:
  case Instruction::AtomicRMW:
  case Instruction::AtomicCmpXchg:
    llvm_unreachable("please use -lower-atomic!");
    exit(1);
  case Instruction::Load: {
    Value *LVal = Inst;
    Value *LAddress = Inst->getOperand(0);
    addPtrTo(wrapValue(LAddress), wrapValue(LVal));

    Mask |= (~0);
  } break;
  case Instruction::Store: {
    Value *SVal = Inst->getOperand(0);
    Value *SAddress = Inst->getOperand(1);
    wrapValue(SAddress);
    wrapValue(SVal);
    addPtrTo(wrapValue(SAddress), wrapValue(SVal));

    Mask |= (~0);
  } break;
  case Instruction::GetElementPtr: {
    makeAlias(wrapValue(Inst), handleGEP((GEPOperator *)Inst));

    Mask |= (~0);
  } break;

    // conversion operations
  case Instruction::AddrSpaceCast:
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPTrunc:
  case Instruction::FPExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::BitCast:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr: {
    Value *CastOperand = Inst->getOperand(0);
    makeAlias(wrapValue(Inst), wrapValue(CastOperand));

    //  function pointer cast
    Type *OrigTy = CastOperand->getType();
    Type *CastTy = Inst->getType();

    if (OrigTy->isPointerTy() &&
        OrigTy->getPointerElementType()->isFunctionTy() &&
        CastTy->isPointerTy() &&
        CastTy->getPointerElementType()->isFunctionTy()) {
      combineFunctionGroups((FunctionType *)OrigTy->getPointerElementType(),
                            (FunctionType *)CastTy->getPointerElementType());
    }

    Mask |= (~0);
  } break;

    // other operations
  case Instruction::CallBr:
  case Instruction::Resume:
  case Instruction::LandingPad:
  case Instruction::Invoke:
    // Instead of crashing, just emit a warning and skip these instructions
    errs() << "WARNING: Skipping " << Inst->getOpcodeName() << " instruction. "
           << "Consider using -lower-invoke for better analysis results.\n";
    break;
  case Instruction::Call: {
    auto *CallI = (CallInst *)Inst;
    if (CallI->isInlineAsm())
      break;

    Value *CV = CallI->getCalledOperand();
    std::vector<Value *> Args;
    for (unsigned K = 0; K < CallI->arg_size(); K++) {
      wrapValue(CallI->getArgOperand(K));
      Args.push_back(CallI->getArgOperand(K));
    }

    this->handleInvokeCallInst(CallI, CV, &Args, Parent);

    if (!CallI->getType()->isVoidTy())
      wrapValue(CallI);

    Mask |= (~0);
  } break;
  case Instruction::PHI: {
    auto *Phi = (PHINode *)Inst;
    auto Nums = Phi->getNumIncomingValues();
    for (unsigned K = 0; K < Nums; K++) {
      Value *P = Phi->getIncomingValue(K);
      wrapValue(Inst);
      auto *PV = wrapValue(P);
      makeAlias(wrapValue(Inst), PV);
    }

    Mask |= (~0);
  } break;
  case Instruction::Select: {
    Value *First = ((SelectInst *)Inst)->getTrueValue();
    Value *Second = ((SelectInst *)Inst)->getFalseValue();
    makeAlias(wrapValue(Inst), wrapValue(First));
    wrapValue(Second);
    makeAlias(wrapValue(Inst), wrapValue(Second));

    wrapValue(((SelectInst *)Inst)->getCondition());

    Mask |= (~0);
  } break;
  case Instruction::VAArg: {
    Parent->addVAArg(Inst);
    DyckGraphNode *VAArg = wrapValue(Inst);
    Value *PtrVAArg = Inst->getOperand(0);
    addPtrTo(wrapValue(PtrVAArg), VAArg);

    Mask |= (~0);
  } break;
  case Instruction::ICmp:
  case Instruction::FCmp:
  default:
    break;
  }

  // wrap unhandled operand
  for (unsigned K = 0; K < Inst->getNumOperands(); K++) {
    if (!(Mask & (1 << K))) {
      wrapValue(Inst->getOperand(K));
    }
  }
}

void AAAnalyzer::handleExtractInsertValueInst(Value *AggValue, Type *AggTy,
                                              ArrayRef<unsigned> &Indices,
                                              Value *InsertedOrExtractedValue) {
  auto *ToInOrExVal = wrapValue(InsertedOrExtractedValue);
  auto *CurrentStruct = wrapValue(AggValue);

  for (unsigned int K = 0; K < Indices.size(); K++) {
    assert(AggTy->isAggregateType() && "Error in handleExtractInsertValueInst, "
                                       "not an agg (array/struct) type!");

    if (AggTy->isArrayTy()) {
      if (K == Indices.size() - 1) {
        CurrentStruct = this->makeAlias(CurrentStruct, ToInOrExVal);
      }
      AggTy = ((ArrayType *)AggTy)->getElementType();
    } else {
      assert(AggTy->isStructTy());
      if (K != Indices.size() - 1) {
        CurrentStruct = this->addField(CurrentStruct, Indices[K], nullptr);
      } else {
        CurrentStruct = this->addField(CurrentStruct, Indices[K], ToInOrExVal);
      }
      AggTy = ((StructType *)AggTy)->getTypeAtIndex(Indices[K]);
    }
  }
}

void AAAnalyzer::handleExtractInsertElmtInst(Value *Vec, Value *Elmt) {
  auto *ElmtVer = wrapValue(Elmt);
  auto *VecVer = wrapValue(Vec);
  this->makeAlias(VecVer, ElmtVer);
}

void AAAnalyzer::handleInvokeCallInst(Instruction *Ret, Value *CV,
                                      std::vector<Value *> *Args,
                                      DyckCallGraphNode *Parent) {
  if (isa<Function>(CV)) {
    if (((Function *)CV)->isIntrinsic()) {
      handleInstrinsic((Instruction *)Ret);
    } else {
      this->handleLibInvokeCallInst(Ret, (Function *)CV, Args, Parent);
      Parent->addCommonCall(new CommonCall(Ret, (Function *)CV, Args));
    }
  } else {
    wrapValue(CV);
    if (isa<ConstantExpr>(CV)) {
      Value *CVCopy = CV;
      while (isa<ConstantExpr>(CVCopy) && ((ConstantExpr *)CVCopy)->isCast()) {
        CVCopy = ((ConstantExpr *)CVCopy)->getOperand(0);
      }

      if (isa<Function>(CVCopy)) {
        this->handleLibInvokeCallInst(Ret, (Function *)CVCopy, Args, Parent);
        Parent->addCommonCall(new CommonCall(Ret, (Function *)CVCopy, Args));
      } else {
        auto *PCall = new PointerCall(Ret, CV, Args);
        Parent->addPointerCall(PCall);
      }
    } else if (isa<GlobalAlias>(CV)) {
      Value *CVCopy = CV;
      while (isa<GlobalAlias>(CVCopy)) {
        CVCopy = ((GlobalAlias *)CVCopy)
                     ->getAliasee()
                     ->stripPointerCastsAndAliases();
      }

      if (isa<Function>(CVCopy)) {
        this->handleLibInvokeCallInst(Ret, (Function *)CVCopy, Args, Parent);
        Parent->addCommonCall(new CommonCall(Ret, (Function *)CVCopy, Args));
      } else {
        auto *PCall = new PointerCall(Ret, CV, Args);
        Parent->addPointerCall(PCall);
      }
    } else {
      auto *PCall = new PointerCall(Ret, CV, Args);
      Parent->addPointerCall(PCall);
    }
  }
}

void AAAnalyzer::handleCommonFunctionCall(Call *C, DyckCallGraphNode *Caller,
                                          DyckCallGraphNode *Callee) {
  // for better precise, if callee is an empty function, we do not match the
  // args and parameters.
  if (Callee->getLLVMFunction()->empty())
    return;

  if (auto *CallInstruction = dyn_cast_or_null<CallInst>(C->getInstruction())) {
    // return<->call
    Type *CalledValueTy = CallInstruction->getCalledOperand()->getType();
    assert(CalledValueTy->isPointerTy() &&
           "A called value is not a pointer type!");
    Type *CalledFuncTy = CalledValueTy->getPointerElementType();
    assert(CalledFuncTy->isFunctionTy() &&
           "A called value is not a function pointer type!");

    std::set<Value *> &Rets = Callee->getReturns();
    if (!((FunctionType *)CalledFuncTy)->getReturnType()->isVoidTy()) {
      Type *RetTy = CallInstruction->getType();

      auto RetIt = Rets.begin();
      while (RetIt != Rets.end()) {
        auto *Val = (Value *)*RetIt;
        if (DL->getTypeStoreSize(RetTy) >=
            DL->getTypeStoreSize(Val->getType())) {
          wrapValue(CallInstruction);
          makeAlias(wrapValue(Val), wrapValue(CallInstruction));
        }
        RetIt++;
      }
    }
  }

  // parameter<->arg
  Function *Func = Callee->getLLVMFunction();
  if (!Func->isIntrinsic()) {
    unsigned NumPars = Func->arg_size();
    unsigned NumArgs = C->numArgs();

    unsigned IterationNum = NumPars < NumArgs ? NumPars : NumArgs;

    for (unsigned K = 0; K < IterationNum; K++) {
      auto *Par = Func->getArg(K);
      Value *Arg = C->getArg(K);

      wrapValue(Arg);
      makeAlias(wrapValue(Par), wrapValue(Arg));

      if (DL->getTypeStoreSize(Par->getType()) <
          DL->getTypeStoreSize(Arg->getType())) {
        // the first pair of arg and par that are not type matched can be
        // aliased, but the followings are rarely possible.
        return;
      }
    }

    if (NumArgs > NumPars && Func->isVarArg()) {
      std::vector<Value *> &VarParameters = Callee->getVAArgs();
      unsigned NumVarPars = VarParameters.size();

      for (unsigned int K = NumPars; K < NumArgs; K++) {
        Value *ArgK = C->getArg(K);
        DyckGraphNode *ArgKNode = wrapValue(ArgK);

        for (unsigned J = 0; J < NumVarPars; J++) {
          auto *VarPar = (Value *)VarParameters[J];

          // for var arg function, we only can get var args according to exact
          // types.
          if (DL->getTypeStoreSize(VarPar->getType()) ==
              DL->getTypeStoreSize(ArgK->getType())) {
            ArgKNode = makeAlias(ArgKNode, wrapValue(VarPar));
          }
        }
      }
    }
  }
}

bool AAAnalyzer::handlePointerFunctionCalls(DyckCallGraphNode *Caller,
                                            int Counter) {
  bool Ret = false;

  auto PCIt = Caller->pointer_call_begin();

  // print in console
  unsigned PCTotal = Caller->pointer_call_size();
  unsigned PCCount = 0;
  //	if (PTCALL_TOTAL == 0)
  //		outs() << "Handling indirect calls in Function #" <<
  //FUNCTION_COUNT << "... 100%, 100%. Done!\r";

  while (PCIt != Caller->pointer_call_end()) {
    // print in console
    unsigned Percentage = ((++PCCount) * 100 / PCTotal);
    //		outs() << "Handling indirect calls in Function #" <<
    //FUNCTION_COUNT << "... " << percentage << "%, \r";

    PointerCall *PCall = *PCIt;
    auto *PCalledVal = PCall->getCalledValue();
    Type *FTy = PCalledVal->getType()->getPointerElementType();
    assert(FTy->isFunctionTy() &&
           "Error in AAAnalyzer::handlePointerFunctionCalls!");

    // handle each unhandled, possible function
    std::set<Value *> EquivAndTypeCompSet;
    auto *EquivSet =
        (const std::set<Value *> *)CFLGraph->retrieveDyckVertex(PCalledVal)
            .first->getEquivalentSet();
    std::set<Function *> *Cands =
        this->getCompatibleFunctions((FunctionType *)FTy);
    set_intersection(
        Cands->begin(), Cands->end(), EquivSet->begin(), EquivSet->end(),
        inserter(EquivAndTypeCompSet, EquivAndTypeCompSet.begin()));

    std::set<Value *> UnhandledFunction;
    set_difference(EquivAndTypeCompSet.begin(), EquivAndTypeCompSet.end(),
                   PCall->begin(), PCall->end(),
                   inserter(UnhandledFunction, UnhandledFunction.begin()));

    // print in console
    unsigned CandTotal = UnhandledFunction.size();
    unsigned CandCount = 0;
    if (CandTotal == 0) {
      //			outs() << "Handling indirect calls in Function
      //#" << FUNCTION_COUNT << "... " << "100%, 100%. Done!\r";
      PCIt++;
      continue;
    }

    auto PFIt = UnhandledFunction.begin();
    while (PFIt != UnhandledFunction.end()) {
      auto *MayAliasedFunctioin = (Function *)(*PFIt);
      // print in console
      //            unsigned Rate = ((100 * (++CandCount)) / CandTotal);
      //            if (Percentage == 100 && Rate == 100) {
      //				outs() << "Handling indirect calls in
      //Function #" << FUNCTION_COUNT << "... " << "100%, 100%. Done!\r";
      //            } else {
      //				outs() << "Handling indirect calls in
      //Function #" << FUNCTION_COUNT << "... " << percentage << "%, " << RATE
      //<< "%         \r";
      //            }

      if (!Ret)
        Ret = true;
      PCall->addMayAliasedFunction(MayAliasedFunctioin);
      handleCommonFunctionCall(
          PCall, Caller, DyckCG->getOrInsertFunction(MayAliasedFunctioin));
      handleLibInvokeCallInst(PCall->getInstruction(), MayAliasedFunctioin,
                              &(PCall->getArgs()), Caller);
      PFIt++;
    }

    PCIt++;
  }

  return Ret;
}

void AAAnalyzer::handleLibInvokeCallInst(Value *Ret, Function *F,
                                         const std::vector<Value *> *Args,
                                         DyckCallGraphNode *Parent) {
  // args must be the real arguments, not the parameters.
  if (!F->empty() || F->isIntrinsic())
    return;

  auto FName = F->getName();

  // Special case: pthread functions (not in ptr.spec)
  if (FName == "pthread_getspecific" && Ret && Args->size() == 1) {
    DyckGraphNode *KeyRep = wrapValue(Args->at(0));
    DyckGraphNode *ValRep = wrapValue(Ret);
    KeyRep->addTarget(ValRep, CFLGraph->getOrInsertIndexEdgeLabel(-1));
    return;
  }
  if (FName == "pthread_setspecific" && Args->size() == 2) {
    DyckGraphNode *KeyRep = wrapValue(Args->at(0));
    DyckGraphNode *ValRep = wrapValue(Args->at(1));
    KeyRep->addTarget(ValRep, CFLGraph->getOrInsertIndexEdgeLabel(-1));
    return;
  }
  if (FName == "pthread_create" && Args->size() == 4) {
    std::vector<Value *> XArgs;
    XArgs.push_back(Args->at(3));
    handleInvokeCallInst(nullptr, Args->at(2), &XArgs,
                         DyckCG->getOrInsertFunction(F));
    return;
  }

  // Use spec manager for standard library functions
  auto copyEffects = specManager.getCopyEffects(F);
  for (const auto &copy : copyEffects) {
    if (copy.dstArgIndex >= 0 && copy.srcArgIndex >= 0) {
      if ((size_t)copy.dstArgIndex < Args->size() &&
          (size_t)copy.srcArgIndex < Args->size()) {
        DyckGraphNode *DstPtr = wrapValue(Args->at(copy.dstArgIndex));
        DyckGraphNode *SrcPtr = wrapValue(Args->at(copy.srcArgIndex));

        if (copy.dstIsRegion && copy.srcIsRegion) {
          // Memory copy (e.g., memcpy, strcpy): *dst = *src
          this->makeContentAlias(DstPtr, SrcPtr);
        } else if (!copy.dstIsRegion && !copy.srcIsRegion) {
          // Value copy: dst = src
          this->makeAlias(DstPtr, SrcPtr);
        }

        if (copy.returnsAlias && Ret) {
          if (copy.retArgIndex >= 0 &&
              (size_t)copy.retArgIndex < Args->size()) {
            this->makeAlias(wrapValue(Ret),
                            wrapValue(Args->at(copy.retArgIndex)));
          } else if (!copy.dstIsRegion) {
            this->makeAlias(wrapValue(Ret), DstPtr);
          }
        }
      }
    }
  }

  // Handle return value aliasing for functions like strchr, strstr
  auto retAliases = specManager.getReturnAliasInfo(F);
  if (!retAliases.empty() && Ret) {
    for (const auto &ra : retAliases) {
      if (ra.argIndex >= 0 && (size_t)ra.argIndex < Args->size()) {
        if (ra.isRegion) {
          // Return aliases *arg (content alias)
          this->makeContentAlias(wrapValue(Args->at(ra.argIndex)),
                                 wrapValue(Ret));
        } else {
          // Return aliases arg (pointer alias)
          this->makeAlias(wrapValue(Ret), wrapValue(Args->at(ra.argIndex)));
        }
      }
    }
  }
}

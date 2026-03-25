//
// Created by prophe cheng on 2025/4/1.
//

#include "Alias/FPA/Config.h"
#include "Alias/FPA/MLTAPass.h"

//
// Implementation
//
pair<Type *, int> typeidx_c(Type *Ty, int Idx) { return make_pair(Ty, Idx); }
pair<size_t, int> hashidx_c(size_t Hash, int Idx) {
  return make_pair(Hash, Idx);
}

bool MLTAPass::getTargetsWithLayerType(size_t TyHash, int Idx, FuncSet &FS) {
  // Get the direct funcset in the current layer, which
  // will be further unioned with other targets from type
  // casting
  if (Idx == -1) {
    for (const auto &FSet : typeIdxFuncsMap[TyHash])
      FS.insert(FSet.second.begin(), FSet.second.end());
  } else {
    FS = typeIdxFuncsMap[TyHash][Idx];
    FS.insert(typeIdxFuncsMap[TyHash][-1].begin(),
              typeIdxFuncsMap[TyHash][-1].end());
  }

  return true;
}

// 判断是不是复合类型
bool MLTAPass::isCompositeType(Type *Ty) {
  if (Ty->isStructTy() || Ty->isArrayTy() || Ty->isVectorTy())
    return true;
  else
    return false;
}

// 返回该value的函数指针类型，如果该value不是函数指针，那么返回NULL
Type *MLTAPass::getFuncPtrType(Value *V) {
  Type *Ty = V->getType();
  if (PointerType *PTy = dyn_cast<PointerType>(Ty)) {
    Type *ETy = PTy->getPointerElementType();
    if (ETy->isFunctionTy())
      return ETy;
  }

  return NULL;
}

// 获取value的base type
Value *MLTAPass::recoverBaseType(Value *V) {
  if (Instruction *I = dyn_cast<Instruction>(V)) {
    map<Value *, Value *> &AliasMap = AliasStructPtrMap[I->getFunction()];
    // 如果是int8*类型并且被cast到其它复杂数据指针类型
    if (AliasMap.find(V) != AliasMap.end())
      return AliasMap[V];
  }
  return NULL;
}

// 要么初始值是function，要么一直cast，不过这里没有考虑ptrtoint的情况了。
Function *MLTAPass::getBaseFunction(Value *V) {
  if (Function *F = dyn_cast<Function>(V))
    if (!F->isIntrinsic())
      return F;

  Value *CV = V;

  // 函数指针可能被cast到其它类型的函数指针
  // 比如fptr_int f = (fptr_int)&f1对应的IR为 store void (i32)* bitcast (void
  // (i64)* @f1 to void (i32)*), void (i32)** %f
  while (BitCastOperator *BCO = dyn_cast<BitCastOperator>(CV)) {
    Value *O = BCO->getOperand(0);
    if (Function *F = dyn_cast<Function>(O))
      if (!F->isIntrinsic())
        return F;
    CV = O;
  }
  return NULL;
}

void MLTAPass::escapeType(Value *V) {
  list<typeidx_t> TyChain;
  bool Complete = true;
  getBaseTypeChain(TyChain, V, Complete);
  for (auto T : TyChain) {
    DBG << "[Escape] Type: " << *(T.first) << "; Idx: " << T.second << "\n";
    typeEscapeSet.insert(Ctx->util.typeIdxHash(T.first, T.second));
  }
}

// 假设var1.f1.f2 = (FromTy)var.
// 那么typeIdxPropMap[type(var1)][f1_idx], typeIdxPropMap[type(var1.f2)][f2_idx]
// add (FromTy, Idx)
void MLTAPass::propagateType(Value *ToV, Type *FromTy, int Idx) {
  list<typeidx_t> TyChain;
  bool Complete = true;
  getBaseTypeChain(TyChain, ToV, Complete); // 获取ToV的type chain
  DBG << "From type: " << getInstructionText(FromTy) << "\n";
  DBG << "To Value: " << getInstructionText(ToV) << "\n";

  for (auto T : TyChain) {
    // 如果type和From Type匹配
    if (Ctx->util.typeHash(T.first) == Ctx->util.typeHash(FromTy) &&
        T.second == Idx)
      continue;

    typeIdxPropMap[Ctx->util.typeHash(T.first)][T.second].insert(
        hashidx_c(Ctx->util.typeHash(FromTy), Idx));
    DBG << "[PROP] " << *(FromTy) << ": " << Idx << "\n\t===> " << *(T.first)
        << " " << T.second << "\n";
  }
}

// This function is to get the base type in the current layer.
// To get the type of next layer (with GEP and Load), use
// nextLayerBaseType() instead.
// 这里有个trick，对结构体类型返回其类型，如果是primitive
// type或者函数指针类型，getBaseType返回null
Type *MLTAPass::getBaseType(Value *V, set<Value *> &Visited) {
  if (!V)
    return NULL;

  if (Visited.find(V) != Visited.end())
    return NULL;
  Visited.insert(V);

  Type *Ty = V->getType();

  if (isCompositeType(Ty))
    return Ty;

  // The value itself is a pointer to a composite type
  else if (Ty->isPointerTy()) {
    Type *ETy = Ty->getPointerElementType();
    // 如果是复杂数据结构指针类型
    if (isCompositeType(ETy))
      return ETy;
    // 如果该value是void*类型并且被cast到其它复合数据类型，返回cast后的类型
    else if (Value *BV = recoverBaseType(V))
      return BV->getType()->getPointerElementType();
  }

  if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(V))
    return getBaseType(BCO->getOperand(0), Visited); // return source type

  else if (SelectInst *SelI = dyn_cast<SelectInst>(V))
    // Assuming both operands have same type, so pick the first
    // operand
    return getBaseType(SelI->getTrueValue(), Visited);

  else if (PHINode *PN = dyn_cast<PHINode>(V))
    // TODO: tracking incoming values
    return _getPhiBaseType(PN, Visited);

  else if (LoadInst *LI = dyn_cast<LoadInst>(V))
    return getBaseType(LI->getPointerOperand(), Visited);

  else if (Type *PTy = dyn_cast<PointerType>(Ty)) {
    // ??
  } else {
  }
  return NULL;
}

// Get the chain of base types for V
// Complete: whether the chain's end is not escaping --- it won't
// propagate further
// Chain: 保存value V的type chain， V：被赋值的value，Complete：分析是否完备
bool MLTAPass::getBaseTypeChain(list<typeidx_t> &Chain, Value *V,
                                bool &Complete) {
  Complete = true;
  Value *CV = V, *NextV = NULL;
  list<typeidx_t> TyList;
  set<Value *> Visited;

  Type *BTy = getBaseType(V, Visited);
  if (BTy) {
    // 0 vs. -1?
    Chain.push_back(typeidx_c(BTy, 0));
  }
  Visited.clear();
  // 沿着top-level variable的def-use chain进行分析
  while (nextLayerBaseType(CV, TyList, NextV, Visited))
    CV = NextV;

  for (auto TyIdx : TyList)
    Chain.push_back(typeidx_c(TyIdx.first, TyIdx.second));

  // Checking completeness
  if (!NextV) {
    Complete = false;
  }

  else if (isa<Argument>(NextV) && NextV->getType()->isPointerTy()) {
    Complete = false;
  }

  else {
    for (auto *U : NextV->users()) {
      if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
        if (NextV == SI->getPointerOperand()) {
          Complete = false;
          break;
        }
      }
    }
    // TODO: other cases like store?
  }

  if (!Chain.empty() && !Complete) {
    DBG << "add escape type in get base chain: "
        << getTypeInfo(Chain.back().first) << "\n";
    typeCapSet.insert(Ctx->util.typeHash(Chain.back().first));
  }

  return true;
}

Type *MLTAPass::_getPhiBaseType(PHINode *PN, set<Value *> &Visited) {
  for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
    Value *IV = PN->getIncomingValue(i);

    Type *BTy = getBaseType(IV, Visited);
    if (BTy)
      return BTy;
  }

  return NULL;
}

// Get the composite type of the lower layer. Layers are split by memory loads
// or GEP 沿着top-level variable的def-use chain进行追踪 V:
// 当前层次的value，NextV：保存下一层value，TyList：保存当前value的类型层次
// 需要注意的是在LLVM编译的时候，有的连续field访问比如
// b.a.func，有时只对应1个getelementptr指令，有时会处理多个。
// 一次nextLayerBaseType最多处理到一个getelementptr，如果有多个需要用while循环处理
bool MLTAPass::nextLayerBaseType(Value *V, list<typeidx_t> &TyList,
                                 Value *&NextV, set<Value *> &Visited) {
  if (!V || isa<Argument>(V)) {
    NextV = V;
    return false;
  }

  if (Visited.find(V) != Visited.end()) {
    NextV = V;
    return false;
  }
  Visited.insert(V);

  // The only way to get the next layer type: GetElementPtrInst or GEPOperator
  // gep格式为getelementptr inbounds %struct.ST, ptr %s, f1, f2, f3, f4, ...
  if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
    NextV = GEP->getPointerOperand(); // 返回s
    bool ret = getGEPLayerTypes(GEP, TyList);
    if (!ret)
      NextV = NULL;
    return ret;
  }
  // 如果是load指令
  else if (LoadInst *LI = dyn_cast<LoadInst>(V)) {
    NextV = LI->getPointerOperand();
    // 求基地址的结构体层次
    return nextLayerBaseType(LI->getOperand(0), TyList, NextV, Visited);
  } else if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(V)) {
    NextV = BCO->getOperand(0); // 求source type的层次
    return nextLayerBaseType(BCO->getOperand(0), TyList, NextV, Visited);
  }
  // Phi and Select
  else if (PHINode *PN = dyn_cast<PHINode>(V)) {
    // FIXME: tracking incoming values
    bool ret = false;
    set<Value *> NVisited;
    list<typeidx_t> NTyList;
    for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
      Value *IV = PN->getIncomingValue(i);
      NextV = IV;
      NVisited = Visited;
      NTyList = TyList;
      ret = nextLayerBaseType(IV, NTyList, NextV, NVisited);
      if (NTyList.size() > TyList.size())
        break;
    }
    TyList = NTyList;
    Visited = NVisited;
    return ret;
  } else if (SelectInst *SelI = dyn_cast<SelectInst>(V)) {
    // Assuming both operands have same type, so pick the first
    // operand
    NextV = SelI->getTrueValue();
    return nextLayerBaseType(SelI->getTrueValue(), TyList, NextV, Visited);
  }
  // Other unary instructions
  // FIXME: may introduce false positives
  else if (UnaryOperator *UO = dyn_cast<UnaryOperator>(V)) {
    NextV = UO->getOperand(0);
    return nextLayerBaseType(UO->getOperand(0), TyList, NextV, Visited);
  }

  NextV = NULL;
  return false;
}

bool MLTAPass::getGEPLayerTypes(GEPOperator *GEP, list<typeidx_t> &TyList) {
  Value *PO = GEP->getPointerOperand();    // base结构体变量
  Type *ETy = GEP->getSourceElementType(); // 通常是base结构体类型

  vector<int> Indices;
  list<typeidx_t> TmpTyList;
  // FIXME: handle downcasting: the GEP may get a field outside the base type Or
  // use O0 to avoid this issue
  ConstantInt *ConstI = dyn_cast<ConstantInt>(GEP->idx_begin()->get());
  if (ConstI && ConstI->getSExtValue() != 0) {
    // FIXME: The following is an attempt to handle the intentional
    // out-of-bound access; however, it is not fully working, so I
    // skip it for now
    Instruction *I = dyn_cast<Instruction>(PO);
    Value *BV = recoverBaseType(PO);
    // 如果是int8*指针cast到其它指针
    if (BV) {
      // 获取被cast到的指针类型
      ETy = BV->getType()->getPointerElementType();
      APInt Offset(ConstI->getBitWidth(), ConstI->getZExtValue());
      Type *BaseTy = ETy;
      SmallVector<APInt> IndiceV =
          DLMap[I->getModule()]->getGEPIndicesForOffset(BaseTy, Offset);
      for (auto Idx : IndiceV)
        Indices.push_back(*Idx.getRawData());
    } else if (StructType *STy = dyn_cast<StructType>(ETy)) {
      bool OptGEP = false;
      for (auto *User : GEP->users()) {
        if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(User)) {
          OptGEP = true;
          // TODO: This conservative decision results may cases
          // disqualifying MLTA. Need an analysis to recover the base
          // types, or use O0 to avoid the optimization
          // return false;
        }
      }
    }
  }

  // Indices保存getelementptr指令所有的索引，比如getelementptr inbounds
  // %struct.ST, ptr %s, f1, f2, f3, f4 返回f1, f2, f3, f4
  if (Indices.empty()) {
    for (auto *it = GEP->idx_begin(); it != GEP->idx_end(); it++) {
      ConstantInt *ConstII = dyn_cast<ConstantInt>(it->get());
      if (ConstII)
        Indices.push_back(ConstII->getSExtValue());
      else
        Indices.push_back(-1);
    }
  }

  // 遍历结构体层次,
  // 这里会忽略第1项，关于第一项介绍参考：https://llvm.org/docs/GetElementPtr.html#what-is-the-first-index-of-the-gep-instruction
  for (auto it = Indices.begin() + 1; it != Indices.end(); it++) {
    int Idx = *it;
    TmpTyList.push_front(typeidx_c(ETy, Idx));
    // Continue to parse subty
    Type *SubTy = NULL;
    if (StructType *STy = dyn_cast<StructType>(ETy))
      SubTy = STy->getElementType(Idx);
    else if (ArrayType *ATy = dyn_cast<ArrayType>(ETy))
      SubTy = ATy->getPointerElementType();
    else if (VectorType *VTy = dyn_cast<VectorType>(ETy))
      SubTy = VTy->getPointerElementType();
    assert(SubTy);
    ETy = SubTy;
  }
  // This is a trouble caused by compiler optimization that
  // eliminates the access path when the index of a field is 0.
  // Conservatively assume a base-struct pointer can serve as a
  // pointer to its first field
  StructType *STy = dyn_cast<StructType>(ETy);
  if (STy && STy->getNumElements() > 0) {
    // Get the type of its first field
    Type *Ty0 = STy->getElementType(0);
    for (auto *U : GEP->users()) {
      if (BitCastOperator *BCO = dyn_cast<BitCastOperator>(U)) {
        if (PointerType *PTy = dyn_cast<PointerType>(BCO->getType())) {
          Type *ToTy = PTy->getPointerElementType();
          if (Ty0 == ToTy)
            TmpTyList.push_front(typeidx_c(ETy, 0));
        }
      }
    }
  }

  if (!TmpTyList.empty()) {
    // Reorder
    for (auto TyIdx : TmpTyList)
      TyList.push_back(TyIdx);
    return true;
  } else
    return false;
}

bool MLTAPass::getDependentTypes(Type *Ty, int Idx, set<hashidx_t> &PropSet) {
  list<hashidx_t> LT;
  LT.push_back(hashidx_c(Ctx->util.typeHash(Ty), Idx));
  set<hashidx_t> Visited;

  while (!LT.empty()) {
    hashidx_t TI = LT.front();
    LT.pop_front();
    if (Visited.find(TI) != Visited.end())
      continue;
    Visited.insert(TI);

    for (hashidx_t Prop : typeIdxPropMap[TI.first][TI.second]) {
      PropSet.insert(Prop);
      LT.push_back(Prop);
    }
    for (hashidx_t Prop : typeIdxPropMap[TI.first][-1]) {
      PropSet.insert(Prop);
      LT.push_back(Prop);
    }
  }
  return true;
}

void MLTAPass::escapeFuncPointer(Value *PO, Instruction *I) { escapeType(PO); }
//
// Created by prophe cheng on 2025/4/1.
//

#include "Alias/FPA/MLTAPass.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Operator.h"

#include "Alias/FPA/Config.h"

bool MLTAPass::doInitialization(Module *M) {
  OP << "#" << MIdx << " Initializing: " << M->getName() << "\n";
  ++MIdx;

  DLMap[M] = &(M->getDataLayout());
  Int8PtrTy[M] = Type::getInt8PtrTy(M->getContext());     // int8 type id: 15
  IntPtrTy[M] = DLMap[M]->getIntPtrType(M->getContext()); // int type id: 13

  set<User *> CastSet;

  // Iterate and process globals，处理该module内的全局变量
  for (Module::global_iterator gi = M->global_begin(); gi != M->global_end();
       ++gi) {
    GlobalVariable *GV = &*gi;

    // 如果该全局变量有初始化操作
    if (GV->hasInitializer()) {
      Type *ITy = GV->getInitializer()->getType();
      if (!ITy->isPointerTy() &&
          !isCompositeType(ITy)) // 如果不是指针类型或者复杂数据类型，跳过
        continue;

      // 保存该全局变量
      Ctx->Globals[GV->getGUID()] = GV;
      // 解析全局变量的initializer
      typeConfineInInitializer(GV);
    }
  }

  // Iterate functions and instructions
  for (Function &F : *M) {
    // Collect address-taken functions.
    // NOTE: declaration functions can also have address taken
    if (F.hasAddressTaken() && !isVirtualFunction(&F)) {
      Ctx->AddressTakenFuncs.insert(&F);
      size_t FuncHash = Ctx->util.funcHash(&F, false);
      // 添加FLTA的结果
      // function的hash，用来进行FLTA，后面可能会修改
      Ctx->sigFuncsMap[FuncHash].insert(&F);
    }

    // The following only considers actual functions with body
    if (F.isDeclaration())
      continue;

    // 计算类型别名信息
    collectAliasStructPtr(&F);
    // 计算结构体field和function之间的约束
    typeConfineInFunction(&F);
    // 类型传播
    typePropInFunction(&F);

    // Collect global function definitions.
    if (F.hasExternalLinkage())
      Ctx->GlobalFuncMap[F.getGUID()] = &F;
  }

  // 处理外部链接的函数
  if (Ctx->Modules.size() == MIdx) {
    // Map the declaration functions to actual ones
    // NOTE: to delete an item, must iterate by reference
    for (auto &SF : Ctx->sigFuncsMap) {
      // 遍历所有的external link function
      for (auto *F : SF.second) {
        if (!F)
          continue;
        // 保留外部链接的函数
        if (F->isDeclaration()) {
          SF.second.erase(F);
          if (Function *AF = Ctx->GlobalFuncMap[F->getGUID()])
            SF.second.insert(AF);
        }
      }
    }

    for (auto &TF : typeIdxFuncsMap) {
      for (auto &IF : TF.second) {
        for (auto *F : IF.second) {
          if (F->isDeclaration()) {
            IF.second.erase(F);
            if (Function *AF = Ctx->GlobalFuncMap[F->getGUID()])
              IF.second.insert(AF);
          }
        }
      }
    }
  }

  return false;
}

void MLTAPass::analyzeIndCall(CallInst *CI, FuncSet *FS) {
  // Initial set: first-layer results
  // TODO: handling virtual functions
  // 获得FLTA结果
  FLTAPass::analyzeIndCall(CI, FS);

  // No need to go through MLTA if the first layer is empty
  if (FS->empty())
    return;

  FuncSet FS1, FS2;
  Type *PrevLayerTy = (dyn_cast<CallBase>(CI))->getFunctionType();
  int PrevIdx = -1;
  // callee expression
  Value *CV = CI->getCalledOperand();
  Value *NextV = NULL;
  int LayerNo = 1;

  // Get the next-layer type
  list<typeidx_t> TyList;
  bool ContinueNextLayer = true;
  DBG << "analyzing call: " << getInstructionText(CI) << "\n";

  auto *Scope = cast<DIScope>(CI->getDebugLoc().getScope());
  string callsiteFile = Scope->getFilename().str();
  int line = CI->getDebugLoc().getLine();
  int col = CI->getDebugLoc().getCol();
  string content = callsiteFile + ":" + itostr(line) + ":" + itostr(col) + "|";

  while (ContinueNextLayer) {
    // Check conditions
    if (LayerNo >= max_type_layer)
      break;

    if (typeCapSet.find(Ctx->util.typeHash(PrevLayerTy)) != typeCapSet.end())
      break;

    set<Value *> Visited;
    nextLayerBaseType(CV, TyList, NextV, Visited);
    if (TyList.empty())
      break;
    // 如果类型层次是B.a(A).f，那么TyList依次为 (A, f), (B, a)
    for (typeidx_t TyIdx : TyList) {
      if (LayerNo >= max_type_layer)
        break;

      ++LayerNo;
      size_t TyIdxHash = Ctx->util.typeIdxHash(TyIdx.first, TyIdx.second);
      // -1 represents all possible fields of a struct
      size_t TyIdxHash_1 = Ctx->util.typeIdxHash(TyIdx.first, -1);

      // Caching for performance
      if (MatchedFuncsMap.find(TyIdxHash) != MatchedFuncsMap.end())
        FS1 = MatchedFuncsMap[TyIdxHash];
      else {
        // CurType ∈ escaped-type
        if (typeEscapeSet.find(TyIdxHash) != typeEscapeSet.end())
          break;
        if (typeEscapeSet.find(TyIdxHash_1) != typeEscapeSet.end())
          break;
        getTargetsWithLayerType(Ctx->util.typeHash(TyIdx.first), TyIdx.second,
                                FS1);
        // Collect targets from dependent types that may propagate
        // targets to it
        set<hashidx_t> PropSet;
        getDependentTypes(TyIdx.first, TyIdx.second, PropSet);
        // for each PropType in type-propagation[CurType] do
        for (auto Prop : PropSet) {
          // 如果存在fromTypeIdx --> curTypeIDx
          getTargetsWithLayerType(Prop.first, Prop.second, FS2);
          FS1.insert(FS2.begin(), FS2.end());
        }
        MatchedFuncsMap[TyIdxHash] = FS1;
      }

      // Next layer may not always have a subset of the previous layer
      // because of casting, so let's do intersection
      intersectFuncSets(FS1, *FS, FS2); // FS2 = FS & FS1
      if (FS2.size() == 0)
        break;
      *FS = FS2; // FS = FS & FS1
      CV = NextV;

      // 如果出现了层次结构体赋值，比如test13中的b.a = a2;
      // 此时B::a并不会confine到function，应该被标记为escaped，但是B::a不是函数指针field。
      // 因此将B标注为escaped type就有必要。
      if (typeCapSet.find(Ctx->util.typeHash(TyIdx.first)) !=
          typeCapSet.end()) {
        ContinueNextLayer = false;
        DBG << "found escaped type: " << getInstructionText(TyIdx.first)
            << " stop\n";
        break;
      }

      PrevLayerTy = TyIdx.first;
      PrevIdx = TyIdx.second;
    }
    TyList.clear();
  }

  if (LayerNo > 1) {
    Ctx->NumSecondLayerTypeCalls++;
    Ctx->NumSecondLayerTargets += FS->size();
  } else {
    Ctx->NumFirstLayerTargets +=
        Ctx->sigFuncsMap[Ctx->util.callHash(CI)].size();
    Ctx->NumFirstLayerTypeCalls += 1;
  }
}

// 分析全局变量并收集function被分配给了哪些type，分析是field sensitive的
bool MLTAPass::typeConfineInInitializer(GlobalVariable *GV) {
  DBG << "Evaluation for global variable: " << GV->getName().str() << "\n";
  // 获取initializer信息
  Constant *Ini = GV->getInitializer();
  // 不是聚合常量，跳过。Aggregate
  // Constants（聚合常量）是指由多个元素组成的常量值，如结构体（struct）或数组（array）。
  // 聚合常量是LLVM IR的一种表示形式，用于表示高级语言中的复合数据类型。
  if (!isa<ConstantAggregate>(Ini))
    return false;

  list<pair<Type *, int>> NestedInit;
  map<Value *, pair<Value *, int>>
      ContainersMap; // key为value->first的第value->second个子常量
  set<Value *> FuncOperands;
  list<User *> LU;
  set<Value *> Visited;
  LU.push_back(Ini);

  // BFS自顶向下访问聚合常量
  while (!LU.empty()) {
    User *U = LU.front();
    LU.pop_front();
    // 如果该聚合常量访问过，跳过
    if (Visited.find(U) != Visited.end())
      continue;
    Visited.insert(U);
    // 获取聚合常量的类型
    Type *UTy = U->getType();
    assert(!UTy->isFunctionTy());
    // 如果是结构体类型的聚合常量并且子常量数量大于0，那么判定常量数量等于结构体field数量
    if (StructType *STy = dyn_cast<StructType>(U->getType())) {
      if (U->getNumOperands() > 0)
        assert(STy->getNumElements() == U->getNumOperands());
      else
        continue;
    }
    // 遍历聚合常量中的每个子常量
    for (auto oi = U->op_begin(), oe = U->op_end(); oi != oe; ++oi) {
      Value *O = *oi;
      Type *OTy = O->getType(); // 该常量的类型
      // oi->getOperandNo为O在聚合常量中的索引，U为父常量
      // 表示O是U的第oi->getOperandNo个子常量
      ContainersMap[O] = make_pair(U, oi->getOperandNo());
      string subConstantText = getInstructionText(O);

      Function *FoundF = NULL; // 当前子常量下的Function Pointer变量
      // Case 1: function address is assigned to a
      // type，如果当前子常量是函数指针
      if (Function *F = dyn_cast<Function>(O))
        FoundF = F; // 如果O是函数类型
      //  a composite-type object (value) is assigned to a
      // field of another composite-type object
      // 如果子常量O仍然是聚合常量，则加入worklist
      else if (isCompositeType(OTy)) {
        // recognize nested composite types
        User *OU = dyn_cast<User>(O);
        LU.push_back(OU);
      }
      // case2: 该常量为pointer cast to int,
      // 也就是将函数指针cast到intptr_t或者uintptr_t类型 比如 1.(int)func
      // 这种将函数地址cast到int 或者为 2.(int)&{...} 将聚合常量地址cast到int
      else if (PtrToIntOperator *PIO = dyn_cast<PtrToIntOperator>(O)) {
        // PIO->getOperand(0)返回ptrtoint的指针变量
        // 如果是函数指针
        Function *FF = dyn_cast<Function>(PIO->getOperand(0));
        if (FF)
          FoundF = FF;
        // 有可能是case4，指向其它全局变量的指针
        else {
          User *OU = dyn_cast<User>(PIO->getOperand(0)); // 如果指针指向聚合常量
          if (isa<GlobalVariable>(PIO->getOperand(0))) {
            if (PIO->getOperand(0)->getType()->isStructTy())
              typeCapSet.insert(Ctx->util.typeHash(U->getType()));
          } else
            LU.push_back(OU);
        }
      }
      // case3，将函数指针cast到void*或者char*类型
      // 比如 1.(void*)func 2.(int)&{...} 将聚合常量地址cast到void*
      else if (BitCastOperator *CO = dyn_cast<BitCastOperator>(O)) {
        // Virtual functions will always be cast by inserting the first
        // parameter
        Function *CF = dyn_cast<Function>(CO->getOperand(0));
        if (CF)
          FoundF = CF;
        else {
          // 获取source操作数，有可能是case4，指向其它复杂数据类型全局变量的指针
          User *OU = dyn_cast<User>(CO->getOperand(0));
          // 如果引用到了别的全局变量，直接退出
          if (isa<GlobalVariable>(CO->getOperand(0))) {
            if (CO->getOperand(0)->getType()->isStructTy())
              typeCapSet.insert(Ctx->util.typeHash(U->getType()));
          } else
            LU.push_back(OU);
        }
      }
      // Case 3: a reference (i.e., pointer) of a composite-type
      // object is assigned to a field of another composite-type
      // object
      // 如果是指针类型
      else if (PointerType *POTy = dyn_cast<PointerType>(OTy)) {
        // 如果是NULL
        if (isa<ConstantPointerNull>(O))
          continue;
        // if the pointer points a composite type, conservatively
        // treat it as a type cap (we cannot get the next-layer type
        // if the type is a cap)
        User *OU = dyn_cast<User>(O);
        // 如果指针指向全局变量，比如test4.c中 struct B b = { .a = &ba }; 这种
        if (GlobalVariable *GO = dyn_cast<GlobalVariable>(OU)) {
          DBG << "subconstant: " << subConstantText
              << " point to global variable: " << GO->getName().str() << "\n";
          Type *Ty = POTy->getPointerElementType(); // 获取指针变量的类型
          // FIXME: take it as a confinement instead of a cap
          if (Ty->isStructTy()) {
            typeCapSet.insert(Ctx->util.typeHash(Ty));
          }

        } else
          LU.push_back(OU);
      } else {
        // TODO: Type escaping?
      }

      // Found a function
      if (FoundF && !FoundF->isIntrinsic()) {
        // "llvm.compiler.used" indicates that the linker may touch
        // it, so do not apply MLTA against them
        if (GV->getName() != "llvm.compiler.used")
          StoredFuncs.insert(FoundF);
        // Add the function type to all containers
        Value *CV = O;
        set<Value *> Visited_; // to avoid loop
        while (ContainersMap.find(CV) != ContainersMap.end()) {
          auto Container = ContainersMap[CV];

          Type *CTy = Container.first->getType(); // 父聚合常量的类型
          set<size_t> TyHS; // 所有满足当前层次对应的结构体type的hash
          TyHS.insert(Ctx->util.typeHash(CTy));
          string type_name = getInstructionText(CTy);
          if (StructType *STy = dyn_cast<StructType>(CTy))
            type_name = Ctx->util.getValidStructName(STy);

          for (auto TyH : TyHS) // 遍历所有可以和当前层次类型对应上的类型hash
            typeIdxFuncsMap[TyH][Container.second].insert(FoundF);

          Visited_.insert(CV);
          if (Visited_.find(Container.first) != Visited_.end())
            break;

          CV = Container.first;
        }
      }
    }
  }

  return true;
}

// This function precisely collect alias types for general pointers
// 收集Function F中的cast操作，主要收集char*,
// void*到其它复杂结构指针类型的cast，记录对应变量转换
// 这里FromValue必须是call指令的返回值。也就是某些返回void*类型函数的返回值,主要用来分析Fromvalue的类型。
// AliasMap必须保证cast后类型的唯一性
void MLTAPass::collectAliasStructPtr(Function *F) {
  map<Value *, Value *> &AliasMap = AliasStructPtrMap[F];
  set<Value *> ToErase;
  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
    Instruction *I = &*i;
    // 遍历所有的cast情况
    if (CastInst *CI = dyn_cast<CastInst>(I)) {
      Value *FromV = CI->getOperand(0);
      // TODO: we only consider calls for now
      if (!isa<CallInst>(FromV)) // FromTy是函数调用返回值
        continue;

      Type *FromTy = FromV->getType(); // 原始类型
      Type *ToTy = CI->getType();      // 转换后类型
      // 必须是从void*类型指针cast到结构体类型
      if (Int8PtrTy[F->getParent()] != FromTy)
        continue;
      // 转换后不是指针类型，跳过
      if (!ToTy->isPointerTy())
        continue;
      if (!isCompositeType(
              ToTy->getPointerElementType())) // 如果ToTy的基类型不是复杂类型
        continue;

      if (AliasMap.find(FromV) != AliasMap.end()) { // FromV已有其它ToV
        ToErase.insert(FromV);
        continue;
      }
      AliasMap[FromV] = CI;
    }
  }

  for (auto *Erase : ToErase) // 不考虑多重重名的类型
    AliasMap.erase(Erase);
}

// 分析结构体field和address-taken function之间的约束
bool MLTAPass::typeConfineInFunction(Function *F) {
  DBG << "analyzing type confine in function: " << F->getName().str() << "\n";
  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
    Instruction *I = &*i;
    // 如果是store
    if (StoreInst *SI = dyn_cast<StoreInst>(I))
      typeConfineInStore(SI);
    // 访问所有参数包含了函数指针的函数调用
    else if (CallInst *CI = dyn_cast<CallInst>(I)) {
      // 访问实参
      Value *CV = CI->getCalledOperand();
      Function *CF = dyn_cast<Function>(CV); // CF为被调用的函数
      for (User::op_iterator OI = CI->op_begin(), OE = CI->op_end(); OI != OE;
           ++OI) {
        // 如果该参数为函数指针
        if (Function *FF = dyn_cast<Function>(
                *OI)) { // 如果该参数是个函数对象，即将函数指针作为参数
          if (FF->isIntrinsic())
            continue;
          // 如果callsite是indirect-call, 不太确定这段代码的效果
          if (CI->isIndirectCall()) {
            confineTargetFunction(*OI, FF); // 将实参的类型和F绑定
            continue;
          }
          // 如果不是间接调用，接着分析
          if (!CF)
            continue;
          // call target
          if (CF->isDeclaration())
            CF = Ctx->GlobalFuncMap[CF->getGUID()];
          if (!CF)
            continue;
          // Arg为函数指针对应的形参
          if (Argument *Arg = Ctx->util.getParamByArgNo(
                  CF,
                  OI->getOperandNo())) { // CF为被调用的函数，这里返回函数指针对应的形参
            // U为函数CF中使用了该形参的指令
            // 遍历所有使用形参的指令
            for (auto *U : Arg->users()) {
              // 如果使用形参的是store指令或者bitcast指令
              if (StoreInst *_SI = dyn_cast<StoreInst>(U))
                confineTargetFunction(_SI->getPointerOperand(), FF);
              else if (isa<BitCastOperator>(U))
                confineTargetFunction(U, FF);
            }
          }
        }
      }
    }
  }

  return true;
}

void MLTAPass::typeConfineInStore(StoreInst *SI) {
  Value *PO = SI->getPointerOperand();
  Value *VO = SI->getValueOperand();

  // 被store的是个function
  Function *CF = getBaseFunction(VO->stripPointerCasts());
  if (!CF)
    return;
  // ToDo: verify this is F or CF
  if (CF->isIntrinsic())
    return;

  confineTargetFunction(PO, CF);
}

// cast有3种情况：
// 1.store ptr value 2.结构体赋值 3.cast type1->type2
bool MLTAPass::typePropInFunction(Function *F) {
  // Two cases for propagation: store and cast.
  // For store, LLVM may use memcpy
  set<User *> CastSet;
  // 遍历F中所有的store指令
  for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
    Instruction *I = &*i;
    Value *PO = NULL, *VO = NULL;

    // case1: store
    // *PO = VO
    if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
      PO = SI->getPointerOperand(); // store的指针变量，dest
      VO = SI->getValueOperand();   // 被store的value，也就是函数地址，source
      DBG << "store inst: " << getInstructionText(SI) << "\n";
    }
    // case2: 用聚合常量给结构体变量赋值
    else if (CallInst *CI = dyn_cast<CallInst>(I)) {
      Value *CV = CI->getCalledOperand();
      Function *CF = dyn_cast<Function>(CV); // called function
      if (CF) {                              // 如果是直接调用
        // LLVM may optimize struct assignment into a call to
        // intrinsic memcpy
        if (CF->getName() == "llvm.memcpy.p0i8.p0i8.i64") {
          PO = CI->getOperand(0); // dest指向结构体变量
          VO = CI->getOperand(1); // source指向聚合常量
          DBG << "memcpy store: " << getInstructionText(CI) << "\n";
        }
      }
    }
    // *PO = VO
    if (PO && VO) {
      // TODO: if VO is a global with an initializer, this should be
      // taken as a confinement instead of propagation, which can
      // improve the precision
      // 不分析聚合常量以及普通常量数据
      if (isa<ConstantAggregate>(VO) || isa<ConstantData>(VO))
        continue;

      list<typeidx_t> TyList;
      Value *NextV = NULL;
      set<Value *> Visited;
      nextLayerBaseType(VO, TyList, NextV, Visited);
      if (!TyList.empty()) {
        for (auto TyIdx : TyList)
          propagateType(PO, TyIdx.first, TyIdx.second);
        continue;
      }

      Visited.clear();
      // source操作数基类型
      Type *BTy = getBaseType(VO, Visited);
      // Composite type，可能对应llvm.memcpy函数
      if (BTy) {
        propagateType(PO, BTy);
        continue;
      }

      Type *FTy = getFuncPtrType(VO->stripPointerCasts());
      // Function-pointer type
      if (FTy) {
        if (!getBaseFunction(VO)) {
          // 从FTy cast到PO
          propagateType(PO, FTy);
          // PO takes function pointer variable instead of function constant,
          // should be deemed escaped.
          escapeFuncPointer(PO, I);
          continue;
        } else
          continue;
      }
      // 如果被store的变量VO不是指针类型，则跳过
      if (!VO->getType()->isPointerTy())
        continue;
      else
        // 如果到这步说明VO不是常量、
        // General-pointer type for escaping
        escapeType(PO);
    }

    // case3: Handle casts
    if (CastInst *CastI = dyn_cast<CastInst>(I))
      // Record the cast, handle later
      CastSet.insert(CastI);

    // Operands of instructions can be BitCastOperator
    for (User::op_iterator OI = I->op_begin(), OE = I->op_end(); OI != OE;
         ++OI) {
      if (BitCastOperator *CO = dyn_cast<BitCastOperator>(*OI))
        CastSet.insert(CO);
    }
  }

  for (User *Cast : CastSet) {
    // TODO: we may not need to handle casts as casts are already
    // stripped out in confinement and propagation analysis. Also for
    // a function pointer to propagate, it is supposed to be stored
    // in memory.

    // The conservative escaping policy can be optimized，cast from struct type1
    // to struct type2
    Type *FromTy = Cast->getOperand(0)->getType();
    Type *ToTy = Cast->getType();

    // Update escaped-type set
    if (FromTy->isPointerTy() && ToTy->isPointerTy()) {
      // 可能有多层指针
      Type *EFromTy = FromTy->getPointerElementType();
      while (EFromTy->isPointerTy())
        EFromTy = EFromTy->getPointerElementType();

      Type *EToTy = ToTy->getPointerElementType();
      while (EToTy->isPointerTy())
        EToTy = EToTy->getPointerElementType();
      // struct pointer to void*, int*, char*
      if (EFromTy->isStructTy() &&
          (EToTy->isVoidTy() || EToTy->isIntegerTy())) {
        typeCapSet.insert(Ctx->util.typeHash(EFromTy));

      }
      // int*, char* to struct*
      else if (EToTy->isStructTy() &&
               (EFromTy->isVoidTy() || EFromTy->isIntegerTy())) {
        typeCapSet.insert(Ctx->util.typeHash(EToTy));
      }
    }

    else if (FromTy->isPointerTy() && ToTy->isIntegerTy()) {
      Type *EFromTy = FromTy->getPointerElementType();
      while (EFromTy->isPointerTy())
        EFromTy = EFromTy->getPointerElementType();

      if (EFromTy->isStructTy())
        typeCapSet.insert(Ctx->util.typeHash(EFromTy));
    }

    else if (ToTy->isPointerTy() && FromTy->isIntegerTy()) {
      Type *EToTy = ToTy->getPointerElementType();
      while (EToTy->isPointerTy())
        EToTy = EToTy->getPointerElementType();

      if (EToTy->isStructTy())
        typeCapSet.insert(Ctx->util.typeHash(EToTy));
    }
  }

  return true;
}

// function F被赋值给了Value V，等于 v = F
void MLTAPass::confineTargetFunction(Value *V, Function *F) {
  if (F->isIntrinsic())
    return;
  StoredFuncs.insert(F);

  list<typeidx_t> TyChain;
  bool Complete = true;
  // 获取变量V的类型层次
  getBaseTypeChain(TyChain, V, Complete);
  for (auto TI : TyChain) {
    DBG << TI.second << " field of Type: " << getInstructionText(TI.first)
        << " add function: " << F->getName().str() << "\n";
    typeIdxFuncsMap[Ctx->util.typeHash(TI.first)][TI.second].insert(F);
  }
  if (!Complete) {
    if (!TyChain.empty()) {
      DBG << "add escape type in confining function : "
          << getTypeInfo(TyChain.back().first) << "\n";
      typeCapSet.insert(Ctx->util.typeHash(TyChain.back().first));
    } else {
      // ToDo: verify is this necessary.
      // typeCapSet.insert(funcHash(F));
    }
  }
}

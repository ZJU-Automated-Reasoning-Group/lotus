//
// Created by prophe cheng on 2025/4/1.
//

#include "Alias/FPA/FLTAPass.h"

bool FLTAPass::doInitialization(Module *M) {
  OP << "#" << MIdx << " Initializing: " << M->getName() << "\n";
  ++MIdx;

  DLMap[M] = &(M->getDataLayout());
  Int8PtrTy[M] = Type::getInt8PtrTy(M->getContext());     // int8 type id: 15
  IntPtrTy[M] = DLMap[M]->getIntPtrType(M->getContext()); // int type id: 13

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
  }

  return false;
}

// 比对两个类型是否相等
bool FLTAPass::fuzzyTypeMatch(Type *Ty1, Type *Ty2, Module *M1, Module *M2) {
  // 如果两个类型一样，直接返回true
  if (Ty1 == Ty2)
    return true;

  // Make the type analysis conservative: assume general
  // pointers, i.e., "void *" and "char *", are equivalent to
  // any pointer type and integer type.
  if ((Ty1 == Int8PtrTy[M1] && (Ty2->isPointerTy() || Ty2 == IntPtrTy[M2])) ||
      (Ty2 == Int8PtrTy[M1] && (Ty1->isPointerTy() || Ty1 == IntPtrTy[M2]))) {
    return true;
  }

  // Conservative: Treat all integer type as same
  if (Ty1->isIntegerTy() && Ty2->isIntegerTy())
    return true;

  // This will change Ty1 and Ty2 hence should follow comparsion like void* and
  // char*
  while (Ty1->isPointerTy() && Ty2->isPointerTy()) {
    Ty1 = Ty1->getPointerElementType();
    Ty2 = Ty2->getPointerElementType();
  }

  // 如果都是结构体且属于同一个结构体类型
  // 修改，不直接比较结构体名字，而是比较结构体hash
  if (Ty1->isStructTy() && Ty2->isStructTy() &&
      (Ctx->util.getValidStructName(dyn_cast<StructType>(Ty1)) ==
       Ctx->util.getValidStructName(dyn_cast<StructType>(Ty2))))
    return true;
  if (Ty1->isIntegerTy() && Ty2->isIntegerTy() &&
      Ty1->getIntegerBitWidth() == Ty2->getIntegerBitWidth())
    return true;
  // TODO: more types to be supported.

  return false;
}

// Find targets of indirect calls based on function-type analysis: as
// long as the number and type of parameters of a function matches
// with the ones of the callsite, we say the function is a possible
// target of this call.
void FLTAPass::findCalleesWithType(CallInst *CI, FuncSet &S) {
  if (CI->isInlineAsm())
    return;
  // Performance improvement: cache results for types
  size_t CIH = Ctx->util.callHash(CI);
  if (MatchedFuncsMap.find(CIH) != MatchedFuncsMap.end()) {
    if (!MatchedFuncsMap[CIH].empty())
      S.insert(MatchedFuncsMap[CIH].begin(), MatchedFuncsMap[CIH].end());
    return;
  }

  CallBase *CB = dyn_cast<CallBase>(CI);
  for (Function *F : Ctx->AddressTakenFuncs) {
    // VarArg （可变参数）
    if (F->getFunctionType()->isVarArg()) {
      // Compare only known args in VarArg.
    }
    // otherwise, the numbers of args should be equal.
    else if (F->arg_size() != CB->arg_size())
      continue;

    if (F->isIntrinsic())
      continue;

    // Types completely match
    if (Ctx->util.callHash(CI) == Ctx->util.funcHash(F)) {
      S.insert(F);
      continue;
    }

    Module *CalleeM = F->getParent();
    Module *CallerM = CI->getFunction()->getParent();

    // Type matching on args.
    bool Matched = true;
    User::op_iterator AI = CB->arg_begin();
    for (Function::arg_iterator FI = F->arg_begin(), FE = F->arg_end();
         FI != FE; ++FI, ++AI) {
      // Check type mis-matches.
      // Get defined type on callee side.
      Type *DefinedTy = FI->getType();
      // Get actual type on caller side.
      Type *ActualTy = (*AI)->getType();

      if (!fuzzyTypeMatch(DefinedTy, ActualTy, CalleeM, CallerM)) {
        Matched = false;
        break;
      }
    }

    // If args are matched, further check return types
    // ToDo: Check whether return type check is needed
    if (Matched) {
      Type *RTy1 = F->getReturnType();
      Type *RTy2 = CI->getType();
      if (!fuzzyTypeMatch(RTy1, RTy2, CalleeM, CallerM))
        Matched = false;
    }

    if (Matched)
      S.insert(F);
  }
  MatchedFuncsMap[CIH] = S;
}

void FLTAPass::analyzeIndCall(CallInst *CI, FuncSet *FS) {
  size_t CIH = Ctx->util.callHash(CI);
  if (MatchedICallTypeMap.find(CIH) != MatchedICallTypeMap.end())
    *FS = MatchedICallTypeMap[CIH];
  else {
    findCalleesWithType(CI, *FS);
    MatchedICallTypeMap[CIH] = *FS;
  }
}
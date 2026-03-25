//
// Created by prophe cheng on 2025/4/6.
//
#include "Alias/FPA/KELPPass.h"

#include "llvm/IR/InstIterator.h"

// 与FLTA和MLTA不同，Kelp这里暂时不把confined address taken
// function添加金Ctx->AddressTakenFuncs
bool KELPPass::doInitialization(Module *M) {
  OP << "#" << MIdx << " Initializing: " << M->getName() << "\n";
  ++MIdx;
  CallGraphPass::doInitialization(M);
  DLMap[M] = &(M->getDataLayout());
  Int8PtrTy[M] = Type::getInt8PtrTy(M->getContext());     // int8 type id: 15
  IntPtrTy[M] = DLMap[M]->getIntPtrType(M->getContext()); // int type id: 13

  set<Function *> potentialConfFuncs;
  set<Value *> totalDefUseSites; // DefUseReachingSites in paper

  OP << "resolving simple function pointer start\n";

  // resolve simple indirect calls and their targets
  for (Function &F : *M) {
    if (F.isDeclaration())
      continue;
    for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i) {
      Instruction *I = &*i;
      if (CallInst *CI = dyn_cast<CallInst>(I)) {
        if (!CI->isIndirectCall())
          continue;
        set<Function *> callees;
        set<Value *> defUseSites;
        set<Function *> visitedFuncs;

        bool flag = resolveSFP(CI, CI->getCalledOperand(), callees, defUseSites,
                               visitedFuncs);

        // simple indirect call
        if (flag) {
          simpleIndCalls.insert(CI);
          Ctx->Callees[CI].insert(callees.begin(), callees.end());
          // mark those function as potential confined functions
          potentialConfFuncs.insert(callees.begin(), callees.end());
          totalDefUseSites.insert(defUseSites.begin(), defUseSites.end());
        }
      }
    }
  }

  OP << "resolving simple function pointer end\n";

  // resolve confined functions
  for (Function *PCF : potentialConfFuncs) {
    bool flag = true;
    for (User *U : PCF->users()) {
      if (CallInst *CI = dyn_cast<CallInst>(U)) {
        // direct call PCF, continue
        if (CI->getCalledOperand() == PCF)
          continue;
      }
      if (!totalDefUseSites.count(U)) {
        flag = false;
        break;
      }
    }

    if (flag)
      confinedAddrTakenFuncs.insert(PCF);
  }

  OP << "resolving confined functions end\n";

  Ctx->NumSimpleIndCalls += simpleIndCalls.size();
  Ctx->NumConfinedFuncs += confinedAddrTakenFuncs.size();

  // MLTA initialization
  set<User *> CastSet;
  // ToDo: 遍历module下所有的结构体，处理别名结构体关系
  // ToDo:
  // 比如%struct.ngx_http_upstream_peer_t.4391和%struct.ngx_http_upstream_peer_t
  // 处理同名结构体
  // Iterate and process globals，处理该module内的全局变量
  unsigned n = 0;
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

  OP << "confine global variable done\n";

  // Iterate functions and instructions
  for (Function &F : *M) {
    // Collect address-taken functions.
    // NOTE: declaration functions can also have address taken
    if (F.hasAddressTaken() && !isVirtualFunction(&F) &&
        !confinedAddrTakenFuncs.count(&F)) {
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

// add confined functions to address-taken function set
bool KELPPass::doFinalization(Module *M) {
  Ctx->AddressTakenFuncs.insert(confinedAddrTakenFuncs.begin(),
                                confinedAddrTakenFuncs.end());
  CallGraphPass::doFinalization(M);
  return false;
}

// resolve complex indirect calls
void KELPPass::analyzeIndCall(CallInst *CI, FuncSet *FS) {
  if (simpleIndCalls.count(CI)) {
    FS->insert(Ctx->Callees[CI].begin(), Ctx->Callees[CI].end());
    return;
  }

  MLTADFPass::analyzeIndCall(CI, FS);
}
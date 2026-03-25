//
// Created by prophe cheng on 2025/4/4.
//
#include "Alias/FPA/MLTADFPass.h"

#include "llvm/IR/InstIterator.h"

void MLTADFPass::typeConfineInStore(StoreInst *SI) {
  Value *PO = SI->getPointerOperand();
  Value *VO = SI->getValueOperand();

  // 被store的是个function
  Function *CF = getBaseFunction(VO->stripPointerCasts());
  if (!CF) {
    set<Function *> Callees;
    set<Value *> UseSites;
    set<Function *> VisitedFuncs;
    bool flag = resolveSFP(SI, VO, Callees, UseSites, VisitedFuncs);
    // none escape
    if (flag) {
      nonEscapeStores.insert(SI);
      for (Function *PF : Callees)
        confineTargetFunction(PO, PF);
    }
    return;
  }
  // ToDo: verify this is F or CF
  if (CF->isIntrinsic())
    return;

  confineTargetFunction(PO, CF);
}

bool MLTADFPass::resolveSFP(Value *User, Value *V, set<Function *> &callees,
                            set<Value *> &defUseSites,
                            set<Function *> &visitedFuncs) {
  if (!V)
    return true;
  if (!justifyUsers(V, User))
    return false;

  // The type of Copy coule be: bitcast, ptrtoint, inttoptr
  if (isa<BitCastInst>(V) || isa<PtrToIntInst>(V) || isa<IntToPtrInst>(V)) {
    Instruction *VI = dyn_cast<Instruction>(V);
    return resolveSFP(VI, VI->getOperand(0), callees, defUseSites,
                      visitedFuncs);
  }
  // nested cast could appear in instructions:
  // for example: %fadd.1 = phi i64 (i32, i32)* [ bitcast (i32 (i32, i32)* @add1
  // to i64 (i32, i32)*), %if.then ], [ %fadd.0, %if.end ]
  else if (isa<BitCastOperator>(V) || isa<PtrToIntOperator>(V)) {
    Operator *O = dyn_cast<Operator>(V);
    return resolveSFP(O, O->getOperand(0), callees, defUseSites, visitedFuncs);
  }

  else if (PHINode *PN = dyn_cast<PHINode>(V)) {
    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
      Value *IV = PN->getIncomingValue(i);
      bool flag = resolveSFP(PN, IV, callees, defUseSites, visitedFuncs);
      if (!flag)
        return false;
    }
    return true;
  }

  else if (Argument *arg = dyn_cast<Argument>(V)) {
    // if current function is address-taken-function,
    // which means the function pointer may be passed throught indirect-call.
    // Hence its hard to find all targets, we conservatively deem it as
    // non-simple indirect call.
    Function *F = arg->getParent();
    visitedFuncs.insert(F);
    if (F->hasAddressTaken())
      return false;
    unsigned argIdx;
    for (argIdx = 0; argIdx < F->arg_size(); ++argIdx)
      if (F->getArg(argIdx) == arg)
        break;
    if (argIdx == F->arg_size())
      return false;

    // Note: recursive call
    for (CallInst *Caller : Ctx->Callers[F]) {
      Function *PF = Caller->getFunction();
      if (visitedFuncs.count(PF))
        continue;
      if (!resolveSFP(Caller, Caller->getArgOperand(argIdx), callees,
                      defUseSites, visitedFuncs))
        return false;
    }
    return true;
  }

  // function pointer: f = getF(...), where getF return function pointer.
  else if (CallInst *CI = dyn_cast<CallInst>(V)) {
    // if function pointer is retrived by indirect call, we conservatively deem
    // it as non-simple function pointer
    if (CI->isIndirectCall())
      return false;
    // handle recursive call
    Function *curF = CI->getFunction();
    visitedFuncs.insert(curF);
    Function *CF = dyn_cast<Function>(CI->getCalledOperand());
    if (!CF)
      return false;
    if (CF->isDeclaration())
      return false;
    if (visitedFuncs.count(CF))
      return true;
    // back traverse from every return inst of CF
    for (inst_iterator i = inst_begin(CF), e = inst_end(CF); i != e; ++i) {
      Instruction *I = &*i;
      if (ReturnInst *RI = dyn_cast<ReturnInst>(I)) {
        if (!resolveSFP(RI, RI->getReturnValue(), callees, defUseSites,
                        visitedFuncs))
          return false;
      }
    }
    return true;
  }

  else if (Function *CF = dyn_cast<Function>(V)) {
    callees.insert(CF);
    defUseSites.insert(User);
    return true;
  }
  // encounter instruction such as: load, store.
  // Conservatively deem it as non-simple indirect call
  else
    return false;
}

bool MLTADFPass::justifyUsers(Value *value, Value *curUser) {
  for (User *user : value->users()) {
    if (user == curUser)
      continue;
    // if function pointer: f is used in CallInst, it is either call: f(xxx) or
    // pass arguments f1(f,...). This is OK
    else if (isa<CallInst>(user))
      continue;
    // function pointer f is not allowed to data flow to other values
    else
      return false;
  }
  return true;
}

void MLTADFPass::escapeFuncPointer(Value *PO, Instruction *I) {
  if (!nonEscapeStores.count(I))
    escapeType(PO);
}
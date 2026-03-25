/*
 * LotusAA - LLVM Compatibility Layer
 * 
 * Provides compatibility across different LLVM versions and platforms.
 * Abstracts away version-specific API differences.
 */

#pragma once

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

namespace llvm {

// CallSite was removed in LLVM 8+, provide compatibility wrapper
#if LLVM_VERSION_MAJOR >= 8

class CallSiteCompat {
  CallBase *CB;

public:
  CallSiteCompat() : CB(nullptr) {}
  CallSiteCompat(Value *V) : CB(dyn_cast_or_null<CallBase>(V)) {}
  CallSiteCompat(CallBase *CB) : CB(CB) {}
  CallSiteCompat(CallInst *CI) : CB(CI) {}
  CallSiteCompat(InvokeInst *II) : CB(II) {}
  CallSiteCompat(Instruction *I) : CB(dyn_cast_or_null<CallBase>(I)) {}

  operator bool() const { return CB != nullptr; }
  bool operator!() const { return CB == nullptr; }
  
  CallBase *getInstruction() const { return CB; }
  Instruction *getInstructionAsValue() const { return CB; }
  
  Function *getCalledFunction() const {
    return CB ? CB->getCalledFunction() : nullptr;
  }
  
  Value *getCalledValue() const {
    return CB ? CB->getCalledOperand() : nullptr;
  }
  
  Value *getCalledOperand() const {
    return CB ? CB->getCalledOperand() : nullptr;
  }
  
  unsigned getNumArgOperands() const {
    return CB ? CB->arg_size() : 0;
  }
  
  Value *getArgOperand(unsigned i) const {
    return CB ? CB->getArgOperand(i) : nullptr;
  }
  
  bool isCall() const { return isa<CallInst>(CB); }
  bool isInvoke() const { return isa<InvokeInst>(CB); }
  
  Type *getType() const { return CB ? CB->getType() : nullptr; }
  FunctionType *getFunctionType() const {
    return CB ? CB->getFunctionType() : nullptr;
  }
};

using CallSite = CallSiteCompat;

#else
#error "LLVM version < 8 not supported"
#endif

// Type compatibility helpers
inline Type *getPointerElementTypeCompat(Type *T, const DataLayout *DL = nullptr) {
  if (PointerType *PT = dyn_cast<PointerType>(T)) {
#if LLVM_VERSION_MAJOR >= 15
    // LLVM 15+ uses opaque pointers by default
    return Type::getInt8Ty(T->getContext());
#else
    return PT->getPointerElementType();
#endif
  }
  return T;
}

inline bool isPointerAnalysisTypeCompatible(Type *lhs, Type *rhs) {
  if (!lhs || !rhs)
    return false;

  if (lhs->isPointerTy() && rhs->isPointerTy())
    return true;

  return lhs == rhs;
}

inline bool isPointerAnalysisCallsiteCompatible(CallBase *call,
                                                Function *callee) {
  if (!call || !callee)
    return false;

  if (!isPointerAnalysisTypeCompatible(call->getType(),
                                       callee->getReturnType())) {
    return false;
  }

  unsigned call_arg_size = call->arg_size();
  unsigned callee_arg_size = callee->arg_size();
  if (!callee->isVarArg() && call_arg_size != callee_arg_size)
    return false;
  if (call_arg_size < callee_arg_size)
    return false;

  unsigned idx = 0;
  for (Argument &formal_arg : callee->args()) {
    if (idx >= call_arg_size)
      return false;
    if (!isPointerAnalysisTypeCompatible(call->getArgOperand(idx)->getType(),
                                         formal_arg.getType())) {
      return false;
    }
    ++idx;
  }

  return true;
}

// DomTree compatibility
using DomTree = DominatorTree;

} // namespace llvm

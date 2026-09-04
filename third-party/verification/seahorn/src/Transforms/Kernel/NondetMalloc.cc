#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Pass.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {
class HandleNondetMalloc : public ModulePass {
public:
  static char ID;

  HandleNondetMalloc() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    Function *nondetMalloc = m.getFunction("nondet.malloc");
    if (!nondetMalloc)
      return false;
    for (CallInst *call : getCalls(nondetMalloc)) {
      for (User *user : call->users()) {
        handleNondetMalloc(user);
      }
      if (hasNullCheck(call)) {
        Value *replace =
            allocNullableType(cast<PointerType>(call->getType()), &m, call);
        replaceMap[call] = replace;
      }
    }
    for (std::pair<Instruction *, Value *> p : replaceMap) {
      p.first->replaceAllUsesWith(p.second);
      p.first->eraseFromParent();
    }
    Function *malloc = m.getFunction("malloc");
    nondetMalloc->replaceAllUsesWith(malloc);
    nondetMalloc->eraseFromParent();
    return true;
  }

  virtual StringRef getPassName() const override {
    return "HandleNondetMalloc";
  }

private:
  DenseMap<Instruction *, Value *> replaceMap;

  void handleNondetMalloc(User *user) {
    if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(user)) {
      insertBeforeNullCheckAddr(gep);
    } else if (ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
      for (CallInst *call : getCalls(ret->getFunction())) {
        for (User *user : call->users()) {
          handleNondetMalloc(user);
        }
      }
    } else if (isa<BitCastOperator>(user)) {
      for (User *u : user->users()) {
        handleNondetMalloc(u);
      }
    }
  }

  void insertBeforeNullCheckAddr(Instruction *inst) {
    for (User *user : inst->users()) {
      insertBeforeNullCheckAddrUser(user);
    }
  }

  void insertBeforeNullCheckAddrUser(User *user) {
    if (LoadInst *load = dyn_cast<LoadInst>(user)) {
      Module *m = load->getModule();
      if (hasNullCheck(load)) {
        PointerType *type = cast<PointerType>(load->getType());
        Value *replace = allocNullableType(type, m, load);
        replaceMap[load] = replace;
      }
    } else if (ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
      for (CallInst *call : getCalls(ret->getFunction())) {
        insertBeforeNullCheckAddr(call);
      }
    } else if (isa<BitCastOperator>(user)) {
      for (User *u : user->users()) {
        insertBeforeNullCheckAddrUser(u);
      }
    }
  }

  Value *allocNullableType(PointerType *type, Module *m, Instruction *before) {
    Function *malloc = m->getFunction("drvhorn.malloc");
    FunctionType *ft =
        FunctionType::get(type, malloc->getArg(0)->getType(), false);
    Constant *casted = ConstantExpr::getBitCast(malloc, ft->getPointerTo());
    size_t size =
        m->getDataLayout().getTypeAllocSize(type->getPointerElementType());
    Type *i64Type = Type::getInt64Ty(m->getContext());
    return CallInst::Create(ft, casted, ConstantInt::get(i64Type, size), "",
                            before);
  }

  bool hasNullCheck(const Instruction *inst) {
    if (!inst->getType()->isPointerTy())
      return false;
    for (const User *user : inst->users()) {
      if (hasNullCheckUser(user))
        return true;
    }
    return false;
  }

  bool hasNullCheckUser(const User *user) {
    if (const ICmpInst *cmp = dyn_cast<ICmpInst>(user)) {
      for (const Value *op : cmp->operands()) {
        if (const Constant *c = dyn_cast<Constant>(op)) {
          if (c->isNullValue())
            return true;
        }
      }
    } else if (const ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
      for (const CallInst *call : getCalls(ret->getFunction())) {
        if (hasNullCheck(call))
          return true;
      }
      return false;
    } else if (isa<BitCastOperator>(user)) {
      for (const User *u : user->users()) {
        if (hasNullCheckUser(u))
          return true;
      }
      return false;
    }
    return false;
  }
};

char HandleNondetMalloc::ID = 0;

Pass *createHandleNondetMallocPass() { return new HandleNondetMalloc(); }
} // namespace seahorn
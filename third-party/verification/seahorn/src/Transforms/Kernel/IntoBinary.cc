#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

class IntoBinary : public ModulePass {
public:
  static char ID;

  IntoBinary() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    // first, make nondet functions binary.
    SmallVector<Function *, 16> nondetFns;
    for (Function &f : m) {
      if (f.isDeclaration() && f.getName().startswith("verifier.nondet")) {
        nondetFns.push_back(&f);
      }
    }
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    for (Function *f : nondetFns) {
      for (CallInst *call : getCalls(f)) {
        DenseMap<const User *, bool> visited;
        if (isOnlyUsedAsBinary(call, visited)) {
          if (!call->getType()->isIntegerTy())
            continue;
          Instruction *newCall = CallInst::Create(ndBool, "", call);
          if (!call->getType()->isIntegerTy(1))
            newCall = new ZExtInst(newCall, call->getType(), "", call);
          call->replaceAllUsesWith(newCall);
          call->eraseFromParent();
        }
      }
    }
    // next, replace the return values of functions that are only used as
    // binary values.
    SmallVector<Function *> binaryFns;
    for (Function &f : m) {
      if (f.isDeclaration() || f.getName().equals("main"))
        continue;
      if (isOnlyUsedAsBinary(&f)) {
        binaryFns.push_back(&f);
      }
    }
    for (Function *f : binaryFns) {
      makeBinary(f);
    }
    runDCEPasses(m, true);
    return true;
  }

  virtual StringRef getPassName() const override { return "IntoBinary"; }

private:
  bool isOnlyUsedAsBinary(const Function *f) {
    if (!f->getReturnType()->isIntegerTy())
      return false;
    for (const CallInst *call : getCalls(f)) {
      DenseMap<const User *, bool> visited;
      if (!isOnlyUsedAsBinary(call, visited)) {
        return false;
      }
    }
    return true;
  }

  bool isOnlyUsedAsBinary(const User *user,
                          DenseMap<const User *, bool> &visited) {
    if (visited.count(user))
      return visited[user];
    if (isa<ICmpInst>(user) || isa<BranchInst>(user)) {
      return true;
    }
    visited[user] = false;
    if (const ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
      for (const CallInst *call : getCalls(ret->getFunction())) {
        if (!isOnlyUsedAsBinary(call, visited))
          return false;
      }
      visited[user] = true;
      return true;
    }
    if (isa<TruncInst, ZExtInst, CallInst, PHINode, BinaryOperator, SelectInst>(
            user)) {
      for (const User *u : user->users()) {
        if (!isOnlyUsedAsBinary(u, visited)) {
          return false;
        }
      }
      visited[user] = true;
      return true;
    }
    return false;
  }

  bool isOperandUsedBinary(const Value *value,
                           DenseMap<const User *, bool> &visited) {
    if (isa<Constant>(value)) {
      return true;
    }
    if (const User *user = dyn_cast<User>(value)) {
      for (const Value *v : user->operands()) {
        if (const User *u = dyn_cast<User>(v)) {
          if (!isOnlyUsedAsBinary(u, visited))
            return false;
        } else if (isa<Argument>(v)) {
          return false;
        }
      }
      return true;
    }
    return false;
  }

  void makeBinary(Function *f) {
    SmallVector<ReturnInst *, 16> ret;
    for (Instruction &inst : instructions(f)) {
      if (ReturnInst *r = dyn_cast<ReturnInst>(&inst)) {
        ret.push_back(r);
      }
    }
    for (ReturnInst *r : ret) {
      makeInstBinary(r);
    }
  }

  void makeInstBinary(Instruction *inst) {
    if (ReturnInst *ret = dyn_cast<ReturnInst>(inst)) {
      makeRetBinary(ret);
    } else if (PHINode *phi = dyn_cast<PHINode>(inst)) {
      makePHIBinary(phi);
    } else if (TruncInst *trunc = dyn_cast<TruncInst>(inst)) {
      if (Instruction *i = dyn_cast<Instruction>(trunc->getOperand(0))) {
        makeInstBinary(i);
      } else {
        errs() << "TODO: makeInstBinary " << *trunc << '\n';
        std::exit(1);
      }
    } else if (PtrToIntInst *ptr2int = dyn_cast<PtrToIntInst>(inst)) {
      Value *ptr = ptr2int->getPointerOperand();
      ICmpInst *eqNull = new ICmpInst(
          inst, CmpInst::Predicate::ICMP_NE, ptr,
          ConstantPointerNull::get(cast<PointerType>(ptr->getType())));
      ZExtInst *zext = new ZExtInst(eqNull, ptr2int->getType(), "", inst);
      ptr2int->replaceAllUsesWith(zext);
      ptr2int->eraseFromParent();
    }
  }

  void makeRetBinary(ReturnInst *ret) {
    Value *val = ret->getReturnValue();
    if (ConstantInt *c = dyn_cast<ConstantInt>(val)) {
      if (!c->isZero()) {
        ReturnInst *newRet = ReturnInst::Create(
            ret->getContext(), ConstantInt::get(c->getType(), 1), ret);
        ret->replaceAllUsesWith(newRet);
        ret->eraseFromParent();
      }
    } else if (Instruction *i = dyn_cast<Instruction>(val)) {
      makeInstBinary(i);
    } else if (isa<Argument>(val)) {
    } else {
      errs() << "TODO: makeRetBinary " << *val << " in "
             << ret->getFunction()->getName() << "\n";
      std::exit(1);
    }
  }

  void makePHIBinary(PHINode *node) {
    for (unsigned i = 0; i < node->getNumIncomingValues(); i++) {
      Value *v = node->getIncomingValue(i);
      if (ConstantInt *c = dyn_cast<ConstantInt>(v)) {
        if (!c->isZero()) {
          node->setIncomingValue(i, ConstantInt::get(c->getType(), 1));
        }
      } else if (Instruction *i = dyn_cast<Instruction>(v)) {
        makeInstBinary(i);
      }
    }
  }

  void runDCEPasses(Module &m, bool removeArg = false) {
    legacy::PassManager pm;
    pm.add(createVerifierPass(false));
    pm.add(createAggressiveDCEPass());
    pm.add(createGlobalDCEPass());
    if (removeArg)
      pm.add(createDeadArgEliminationPass());
    pm.add(createCFGSimplificationPass());
    pm.run(m);
  }
};

char IntoBinary::ID = 0;

Pass *createIntoBinaryPass() { return new IntoBinary(); }
} // namespace seahorn
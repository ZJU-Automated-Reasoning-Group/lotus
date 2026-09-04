#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

class InitGlobalKrefs : public ModulePass {
public:
  static char ID;
  InitGlobalKrefs() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    LLVMContext &ctx = m.getContext();
    StructType *krefType = StructType::getTypeByName(ctx, "struct.kref");

    bool changed = false;
    Function *prelude = Function::Create(
        FunctionType::get(Type::getVoidTy(ctx), false),
        GlobalValue::LinkageTypes::PrivateLinkage, "drvhorn.prelude", m);
    BasicBlock *blk = BasicBlock::Create(ctx, "entry", prelude);
    IRBuilder<> b(blk);

    for (GlobalVariable &gv : m.globals()) {
      Type *t = gv.getValueType();
      if (StructType *st = dyn_cast<StructType>(t)) {
        const Optional<SmallVector<Value *>> &indices =
            gepIndicesToStruct(st, krefType);
        if (indices.hasValue()) {
          std::string krefName = "drvhorn.kref." + st->getName().str();
          initializeTargetGv(&gv, indices.getValue(), krefName, b);
          changed = true;
        }
      } else if (ArrayType *at = dyn_cast<ArrayType>(t)) {
        // currently we only handle array of pointers to structs.
        PointerType *et = dyn_cast<PointerType>(at->getElementType());
        if (!et)
          continue;
        StructType *innerType = dyn_cast<StructType>(et->getElementType());
        if (!innerType)
          continue;
        const Optional<SmallVector<Value *>> &indices =
            gepIndicesToStruct(innerType, krefType);
        if (indices.hasValue()) {
          changed |= handleGlobalArrayElems(gv, b, indices.getValue());
        }
      }
    }
    b.CreateRetVoid();

    if (Function *main = m.getFunction("main")) {
      callPreludeInMain(main, prelude);
      changed = true;
    }
    return changed;
  }

  virtual StringRef getPassName() const override { return "InitGlobalKrefs"; }

private:
  SmallVector<GlobalVariable *> krefsToAssert;

  bool handleGlobalArrayElems(GlobalVariable &gv, IRBuilder<> &b,
                              const SmallVector<Value *> &indices) {
    Module *m = gv.getParent();
    bool ret = false;
    for (User *user : gv.users()) {
      if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(user)) {
        if (isArrayElementAccess(gep)) {
          // since gv is an array of a pointer to a struct, load should follow.
          const SmallVector<LoadInst *> &loads = getElementLoads(gep);
          for (LoadInst *load : loads) {
            Type *t = load->getType()->getPointerElementType();
            std::string name = "drvhorn.kref.arrayelem." + gv.getName().str();
            GlobalVariable *elem = new GlobalVariable(
                *m, t, false, GlobalValue::LinkageTypes::PrivateLinkage,
                Constant::getNullValue(t), name);
            load->replaceAllUsesWith(elem);
            std::string krefName =
                "drvhorn.kref.arrayelem." + gv.getName().str();
            initializeTargetGv(elem, indices, krefName, b);
            replaceNullCheckWithNondet(elem);
            ret = true;
          }
        }
      }
    }
    return ret;
  }

  bool isArrayElementAccess(const GetElementPtrInst *gep) {
    LLVMContext &ctx = gep->getContext();
    if (gep->getNumIndices() != 2)
      return false;
    // The first index should be an i64 0.
    if (gep->getOperand(1) != ConstantInt::get(Type::getInt64Ty(ctx), 0))
      return false;
    // The second index should be an i64.
    return gep->getOperand(2)->getType()->isIntegerTy(64);
  }

  SmallVector<LoadInst *> getElementLoads(GetElementPtrInst *gep) {
    SmallVector<LoadInst *> ret;
    for (User *user : gep->users()) {
      if (LoadInst *load = dyn_cast<LoadInst>(user))
        ret.push_back(load);
    }
    return ret;
  }

  void initializeTargetGv(GlobalVariable *gv,
                          const SmallVector<Value *> &gepIndices,
                          StringRef krefName, IRBuilder<> &b) {
    Module *m = gv->getParent();
    Function *krefInit = m->getFunction("drvhorn.kref_init");
    Function *ndFn = getNondetFn(m, cast<StructType>(gv->getValueType()));
    b.CreateStore(b.CreateCall(ndFn), gv);
    Value *kref =
        b.CreateGEP(gv->getType()->getPointerElementType(), gv, gepIndices);
    b.CreateCall(krefInit, kref);
  }

  void replaceNullCheckWithNondet(GlobalVariable *elem) {
    Module *m = elem->getParent();
    Function *ndBool = getOrCreateNdIntFn(*m, 1);
    SmallVector<ICmpInst *> nullChecks;
    for (User *user : elem->users()) {
      if (ICmpInst *icmp = dyn_cast<ICmpInst>(user)) {
        if (icmp->getOperand(1) == ConstantPointerNull::get(elem->getType())) {
          nullChecks.push_back(icmp);
        }
      }
    }
    for (ICmpInst *icmp : nullChecks) {
      IRBuilder<> b(icmp);
      Value *cond = b.CreateCall(ndBool);
      icmp->replaceAllUsesWith(cond);
      icmp->eraseFromParent();
    }
  }

  Function *getNondetFn(Module *m, StructType *s) {
    std::string name = "verifier.nondetvalue." + s->getName().str();
    if (Function *f = m->getFunction(name))
      return f;
    return Function::Create(FunctionType::get(s, false),
                            GlobalValue::LinkageTypes::ExternalLinkage, name,
                            m);
  }

  void callPreludeInMain(Function *main, Function *prelude) {
    BasicBlock &entry = main->getEntryBlock();
    IRBuilder<> b(&*entry.begin());
    b.CreateCall(prelude);
  }
};

char InitGlobalKrefs::ID = 0;

Pass *createInitGlobalKrefsPass() { return new InitGlobalKrefs(); };
}; // namespace seahorn
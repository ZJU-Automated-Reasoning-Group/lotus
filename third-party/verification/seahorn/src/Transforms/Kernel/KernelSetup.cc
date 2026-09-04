#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Pass.h"

#include "boost/algorithm/string.hpp"
#include "boost/range.hpp"
#include "seahorn/Support/SeaDebug.h"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

class KernelSetup : public ModulePass {
public:
  static char ID;

  KernelSetup() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    m.setModuleInlineAsm("");
    Function *allocStub = getOrCreateAlloc(m);
    stubAllocationFunctions(m, allocStub);
    handleKrefAndKobjectAPIs(m);
    handleKmemCache(m, allocStub);
    handleIsErr(m);
    handleCpuPossibleMask(m);
    return true;
  }

  virtual StringRef getPassName() const override { return "KernelSetup"; }

private:
  DenseMap<const Type *, FunctionCallee> ndfn;

  void stubAllocationFunctions(Module &m, Function *allocStub) {
    struct AllocFn {
      StringRef name;
      unsigned sizeIdx;
    };
    // TODO: handle flags for zero-init.
    AllocFn allocFns[] = {
        {"__kmalloc", 0},
        {"__kmalloc_node", 0},
        {"__kmalloc_node_track_caller", 0},
        {"kmalloc_large", 0},
        {"kmalloc_trace", 2},
        {"kmalloc_large_node", 0},
        {"__vmalloc_node_range", 0},
        {"slob_alloc", 0},
        {"pcpu_alloc", 0},
        {"__ioremap_caller", 1},
        {"__early_ioremap", 1},
        {"devm_kmalloc", 1},
        {"kvmalloc_node", 0},
    };
    for (const AllocFn &f : allocFns) {
      Function *orig = m.getFunction(f.name);
      if (!orig)
        continue;
      for (CallInst *call : getCalls(orig)) {
        IRBuilder<> b(call);
        Value *replace =
            b.CreateCall(allocStub, call->getArgOperand(f.sizeIdx));
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
      }
    }
  }

  Type *getKrefTy(Module &m) {
    Function *krefInit = m.getFunction("kref_init");
    return krefInit->getArg(0)->getType()->getPointerElementType();
  }

  Function *buildKrefInit(Module &m, Type *krefType) {
    LLVMContext &ctx = m.getContext();
    FunctionType *krefInitTy = FunctionType::get(
        Type::getVoidTy(ctx), krefType->getPointerTo(), false);
    Function *krefInit = Function::Create(
        krefInitTy, GlobalValue::ExternalLinkage, "drvhorn.kref_init", &m);
    BasicBlock *blk = BasicBlock::Create(ctx, "", krefInit);
    IRBuilder<> b(blk);
    Value *gep = b.CreateInBoundsGEP(
        krefType, krefInit->getArg(0),
        {b.getInt64(0), b.getInt32(0), b.getInt32(0), b.getInt32(0)});
    b.CreateStore(b.getInt32(1), gep);
    b.CreateRetVoid();

    return krefInit;
  }

  Function *buildKrefGet(Module &m, Type *krefType) {
    LLVMContext &ctx = m.getContext();
    IntegerType *i32Ty = Type::getInt32Ty(ctx);
    FunctionType *krefGetTy = FunctionType::get(
        Type::getVoidTy(ctx), krefType->getPointerTo(), false);
    Function *krefGet = Function::Create(
        krefGetTy, GlobalValue::InternalLinkage, "drvhorn.kref_get", &m);
    BasicBlock *blk = BasicBlock::Create(ctx, "blk", krefGet);
    IRBuilder<> b(blk);
    Value *gep = b.CreateInBoundsGEP(
        krefType, krefGet->getArg(0),
        {b.getInt64(0), b.getInt32(0), b.getInt32(0), b.getInt32(0)});
    LoadInst *load = b.CreateLoad(i32Ty, gep);
    Value *add = b.CreateAdd(load, b.getInt32(1));
    b.CreateStore(add, gep);
    b.CreateRetVoid();
    return krefGet;
  }

  Function *buildKrefPut(Module &m, Type *krefType) {
    LLVMContext &ctx = m.getContext();
    IntegerType *i32Ty = Type::getInt32Ty(ctx);
    FunctionType *krefPutTy =
        FunctionType::get(i32Ty, krefType->getPointerTo(), false);
    Function *krefPut = Function::Create(
        krefPutTy, GlobalValue::InternalLinkage, "drvhorn.kref_put", &m);
    BasicBlock *blk = BasicBlock::Create(ctx, "blk", krefPut);

    IRBuilder<> b(blk);
    Argument *krefPtr = krefPut->getArg(0);
    Value *gep = b.CreateInBoundsGEP(
        krefType, krefPtr,
        {b.getInt64(0), b.getInt32(0), b.getInt32(0), b.getInt32(0)});
    LoadInst *load = b.CreateLoad(i32Ty, gep);
    Value *sub = b.CreateSub(load, b.getInt32(1));
    b.CreateStore(sub, gep);
    Value *isZero = b.CreateICmpEQ(sub, b.getInt32(0));
    Value *retVal = b.CreateZExt(isZero, i32Ty);
    b.CreateRet(retVal);

    return krefPut;
  }

  void handleKrefAndKobjectAPIs(Module &m) {
    Type *krefType = getKrefTy(m);
    Function *krefInit = buildKrefInit(m, krefType);
    Function *krefGet = buildKrefGet(m, krefType);
    Function *krefPut = buildKrefPut(m, krefType);

    auto replaceCalls = [](Function *orig, Function *newFn) {
      for (CallInst *call : getCalls(orig)) {
        IRBuilder<> b(call);
        if (!call->arg_size())
          continue;
        CallInst *newCall = b.CreateCall(newFn, call->getArgOperand(0));
        call->replaceAllUsesWith(newCall);
        call->eraseFromParent();
      }
    };

    for (Function &f : m) {
      StringRef name = f.getName();
      if (name.equals("kref_init") || name.startswith("kref_init.")) {
        replaceCalls(&f, krefInit);
      } else if (name.equals("kref_get") || name.startswith("kref_get.")) {
        replaceCalls(&f, krefGet);
      } else if (name.equals("kref_put") || name.startswith("kref_put.")) {
        replaceCalls(&f, krefPut);
      }
    }

    buildKobjGet(m, krefGet, krefType);
    buildKobjPut(m, krefPut, krefType);
    // TODO: implement kobject_get_unless_zero
  }

  void buildKobjGet(Module &m, Function *krefGet, Type *krefType) {
    Function *kobjGet = m.getFunction("kobject_get");
    if (!kobjGet || !kobjGet->empty())
      return;
    LLVMContext &ctx = m.getContext();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", kobjGet);
    BasicBlock *body = BasicBlock::Create(ctx, "body", kobjGet);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", kobjGet);
    Argument *kobj = kobjGet->getArg(0);
    StructType *kobjType =
        cast<StructType>(kobj->getType()->getPointerElementType());

    IRBuilder<> b(entry);
    Value *isNull = b.CreateIsNull(kobj);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    Value *krefGEP = b.CreateInBoundsGEP(
        kobjType, kobj, gepIndicesToStruct(kobjType, krefType).getValue());
    b.CreateCall(krefGet, krefGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRet(kobj);
  }

  void buildKobjPut(Module &m, Function *krefPut, Type *krefType) {
    Function *kobjPut = m.getFunction("kobject_put");
    if (!kobjPut || !kobjPut->empty())
      return;
    LLVMContext &ctx = m.getContext();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", kobjPut);
    BasicBlock *body = BasicBlock::Create(ctx, "body", kobjPut);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", kobjPut);
    Argument *kobj = kobjPut->getArg(0);
    StructType *kobjType =
        cast<StructType>(kobj->getType()->getPointerElementType());

    IRBuilder<> b(entry);
    Value *isNull = b.CreateIsNull(kobj);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    Value *krefGEP = b.CreateInBoundsGEP(
        kobjType, kobj, gepIndicesToStruct(kobjType, krefType).getValue());
    b.CreateCall(krefPut, krefGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRetVoid();
  }

  GlobalVariable *gVarOfKmemCacheAllocCall(Module &m, CallInst *call) {
    Value *cache = call->getArgOperand(0);
    if (LoadInst *load = dyn_cast<LoadInst>(cache)) {
      if (GlobalVariable *gv =
              dyn_cast<GlobalVariable>(load->getPointerOperand())) {
        return gv;
      }
    }
    return nullptr;
  }

  Optional<size_t> getKmemCacheSize(GlobalVariable *gv) {
    for (User *user : gv->users()) {
      if (StoreInst *store = dyn_cast<StoreInst>(user)) {
        if (store->getPointerOperand() == gv) {
          Value *src = store->getValueOperand();
          if (CallInst *call = dyn_cast<CallInst>(src)) {
            Function *f = call->getCalledFunction();
            if (f && (f->getName() == "kmem_cache_create" ||
                      f->getName() == "kmem_cache_create_usercopy")) {
              ConstantInt *ci = cast<ConstantInt>(call->getArgOperand(1));
              return ci->getZExtValue();
            } else {
              errs() << "TODO: getKmemCacheSize: unhandled function\n";
            }
          } else if (isa<ConstantPointerNull>(src)) {
            return 0;
          } else {
            errs() << "else " << *src << '\n';
          }
        }
      }
    }
    return None;
  }

  void handleKmemCache(Module &m, Function *allocStub) {
    StringRef kmemCacheFuncNames[] = {
        "kmem_cache_alloc",
        "kmem_cache_alloc_lru",
        "kmem_cache_alloc_node",
    };
    LLVMContext &ctx = m.getContext();
    IntegerType *i64Ty = Type::getInt64Ty(ctx);
    for (StringRef name : kmemCacheFuncNames) {
      Function *orig = m.getFunction(name);
      if (!orig)
        continue;
      for (User *user : orig->users()) {
        if (CallInst *call = dyn_cast<CallInst>(user)) {
          GlobalVariable *gv = gVarOfKmemCacheAllocCall(m, call);
          if (!gv) {
            errs() << "TODO: kmem_cache_alloc: global variable not found\n";
            continue;
          }
          Optional<size_t> size = getKmemCacheSize(gv);
          if (!size.hasValue()) {
            continue;
          }
          ConstantInt *sizeArg = ConstantInt::get(i64Ty, size.getValue());
          IRBuilder<> b(call);
          Value *newMalloc = b.CreateCall(allocStub, sizeArg);
          call->replaceAllUsesWith(newMalloc);
        }
      }
    }
  }

  // The IS_ERR check is translated to something like the following LLVM IR:
  //   %1 = icmp ugt %struct.a* %0, inttoptr (i64 -4096 to %struct.a*)
  // This code may be evaluated to true even if %0 is a valid pointer.
  // We replace the IS_ERR check with comparison to 0, i.e.
  //   %1 = icmp eq %struct.a* %0, inttoptr (i64 0 to %struct.a*)
  void handleIsErr(Module &m) {
    auto isErr = [](ICmpInst *icmp) {
      if (icmp->getPredicate() == CmpInst::ICMP_UGT) {
        if (ConstantExpr *ci = dyn_cast<ConstantExpr>(icmp->getOperand(1))) {
          if (ci->getOpcode() == Instruction::IntToPtr) {
            if (ConstantInt *intVal =
                    dyn_cast<ConstantInt>(ci->getOperand(0))) {
              return intVal->getSExtValue() == -4096;
            }
          }
        }
      }
      return false;
    };

    auto isPtrErr = [](const PtrToIntInst *ptrToInt) {
      if (!ptrToInt->getDestTy()->isIntegerTy(64) ||
          !ptrToInt->getFunction()->getReturnType()->isIntegerTy(32))
        return false;
      DenseSet<const User *> visited;
      SmallVector<const User *> users;
      users.push_back(ptrToInt);
      while (!users.empty()) {
        const User *user = users.pop_back_val();
        if (isa<ReturnInst>(user)) {
          return true;
        } else if (isa<PtrToIntInst, TruncInst, PHINode, SelectInst>(user)) {
          for (const User *u : user->users())
            if (visited.insert(u).second)
              users.push_back(u);
        }
      }
      return false;
    };

    SmallVector<Instruction *> toRemove;
    for (Function &f : m) {
      for (Instruction &inst : instructions(f)) {
        if (ICmpInst *icmp = dyn_cast<ICmpInst>(&inst)) {
          if (isErr(icmp)) {
            icmp->setPredicate(CmpInst::ICMP_EQ);
            Constant *rhs =
                Constant::getNullValue(icmp->getOperand(0)->getType());
            icmp->setOperand(1, rhs);
          }
        }
        // inttoptr should be the result of ERR_PTR macro.
        if (IntToPtrInst *intToPtr = dyn_cast<IntToPtrInst>(&inst)) {
          Constant *null = Constant::getNullValue(intToPtr->getDestTy());
          toRemove.push_back(intToPtr);
          intToPtr->replaceAllUsesWith(null);
        }
        if (PtrToIntInst *ptrToInt = dyn_cast<PtrToIntInst>(&inst)) {
          if (isPtrErr(ptrToInt)) {
            IRBuilder<> b(ptrToInt);
            // All errors are represented as -1.
            Value *zero = b.getInt64(-1);
            ptrToInt->replaceAllUsesWith(zero);
            toRemove.push_back(ptrToInt);
          }
        }
      }
    }
    for (Instruction *inst : toRemove) {
      inst->eraseFromParent();
    }
  }

  void handleCpuPossibleMask(Module &m) {
    GlobalVariable *cpuMask = m.getGlobalVariable("__cpu_possible_mask");
    if (!cpuMask)
      return;
    SmallVector<Instruction *> toRemove;
    SmallVector<User *> users;
    DenseSet<User *> visited;
    users.push_back(cpuMask);
    while (!users.empty()) {
      User *user = users.pop_back_val();
      if (!visited.insert(user).second)
        continue;
      if (LoadInst *load = dyn_cast<LoadInst>(user)) {
        FunctionCallee ndFn = getNondetFn(load->getType(), m);
        IRBuilder<> b(load);
        Value *nd = b.CreateCall(ndFn);
        load->replaceAllUsesWith(nd);
        toRemove.push_back(load);
      } else {
        for (User *u : user->users())
          users.push_back(u);
      }
    }

    for (Instruction *inst : toRemove) {
      inst->eraseFromParent();
    }
  }

  FunctionCallee makeNewNondetFn(Module &m, Type &type, unsigned num,
                                 std::string prefix) {
    std::string name;
    unsigned c = num;
    do {
      name = prefix + std::to_string(c++);
    } while (m.getNamedValue(name));
    FunctionCallee res = m.getOrInsertFunction(name, &type);
    return res;
  }

  FunctionCallee getNondetFn(Type *type, Module &m) {
    auto it = ndfn.find(type);
    if (it != ndfn.end()) {
      return it->second;
    }

    FunctionCallee res =
        makeNewNondetFn(m, *type, ndfn.size(), "verifier.nondet.");
    ndfn[type] = res;
    return res;
  }
};

char KernelSetup::ID = 0;

Pass *createKernelSetupPass() { return new KernelSetup(); }
} // namespace seahorn
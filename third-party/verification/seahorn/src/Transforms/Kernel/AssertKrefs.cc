#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

class AssertKrefs : public ModulePass {
public:
  static char ID;

  AssertKrefs() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    Function *krefChecker = buildKrefChecker(m);
    buildFail(m, krefChecker);
    return true;
  }

  virtual StringRef getPassName() const override { return "AssertKrefs"; }

  void getAnalysisUsage(AnalysisUsage &au) const override {
    au.addRequired<SeaBuiltinsInfoWrapperPass>();
    au.setPreservesAll();
  }

private:
  Function *buildKrefChecker(Module &m) {
    LLVMContext &ctx = m.getContext();
    IntegerType *i64Ty = Type::getInt64Ty(ctx);
    IntegerType *i32Ty = Type::getInt32Ty(ctx);
    StructType *krefType = StructType::getTypeByName(ctx, "struct.kref");
    SeaBuiltinsInfo &sbi = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    Function *errFn = sbi.mkSeaBuiltinFn(SeaBuiltinsOp::ERROR, m);
    Function *f = Function::Create(
        FunctionType::get(Type::getVoidTy(ctx), krefType->getPointerTo(),
                          false),
        GlobalValue::PrivateLinkage, "drvhorn.assert_kref", &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *err = BasicBlock::Create(ctx, "err", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Argument *kref = f->getArg(0);
    Value *counterGEP = b.CreateInBoundsGEP(
        krefType, kref,
        {ConstantInt::get(i64Ty, 0), ConstantInt::get(i32Ty, 0),
         ConstantInt::get(i32Ty, 0), ConstantInt::get(i32Ty, 0)});
    Value *counter = b.CreateLoad(i32Ty, counterGEP);
    Value *equals = b.CreateICmpEQ(counter, ConstantInt::get(i32Ty, 1));
    b.CreateCondBr(equals, ret, err);

    b.SetInsertPoint(err);
    b.CreateCall(errFn);
    b.CreateUnreachable();

    b.SetInsertPoint(ret);
    b.CreateRetVoid();
    return f;
  }

  Function *getOrBuildRawcountChecker(Module &m, IntegerType *iTy,
                                      uint64_t correctCount) {
    std::string name =
        "drvhorn.assert_rawcount.i" + std::to_string(iTy->getBitWidth());
    if (Function *f = m.getFunction(name))
      return f;
    LLVMContext &ctx = m.getContext();
    SeaBuiltinsInfo &sbi = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    Function *errFn = sbi.mkSeaBuiltinFn(SeaBuiltinsOp::ERROR, m);
    Function *f =
        Function::Create(FunctionType::get(Type::getVoidTy(ctx), iTy, false),
                         GlobalValue::PrivateLinkage, name, &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *err = BasicBlock::Create(ctx, "err", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Argument *val = f->getArg(0);
    Value *equals = b.CreateICmpEQ(val, ConstantInt::get(iTy, correctCount));
    b.CreateCondBr(equals, ret, err);

    b.SetInsertPoint(err);
    b.CreateCall(errFn);
    b.CreateUnreachable();

    b.SetInsertPoint(ret);
    b.CreateRetVoid();
    return f;
  }

  void buildFail(Module &m, Function *krefChecker) {
    Function *fail = m.getFunction("drvhorn.fail");
    if (!fail)
      return;
    LLVMContext &ctx = m.getContext();
    BasicBlock *blk = BasicBlock::Create(ctx, "entry", fail);
    StructType *krefType = StructType::getTypeByName(ctx, "struct.kref");

    IRBuilder<> b(blk);
    if (fail->arg_size()) {
      Argument *instance = fail->arg_begin();
      checkInstance(m, instance, b, ctx);
    }

    static const std::string storagePrefix = "drvhorn.storage.";
    for (GlobalVariable &gv : m.globals()) {
      if (gv.getName().startswith(storagePrefix)) {
        StringRef suffix = gv.getName().substr(storagePrefix.size());
        GlobalVariable *targetIndex =
            m.getGlobalVariable("drvhorn.target_index." + suffix.str(), true);
        Function *f =
            genAssertFunction(m, krefChecker, gv, targetIndex, suffix);
        b.CreateCall(f);
      } else if (gv.getName().startswith("drvhorn.kref.")) {
        Type *type = gv.getValueType();
        Value *krefPtr;
        if (equivTypes(type, krefType->getPointerTo())) {
          krefPtr = b.CreateLoad(type, &gv);
        } else {
          krefPtr = b.CreateInBoundsGEP(
              type, &gv,
              gepIndicesToStruct(cast<StructType>(type), krefType).getValue());
        }
        b.CreateCall(krefChecker, krefPtr);
      }
    }

    b.CreateRetVoid();
  }

  void checkInstance(Module &m, Argument *instance, IRBuilder<> &b,
                     LLVMContext &ctx) {
    StructType *instanceType =
        cast<StructType>(instance->getType()->getPointerElementType());
    StructType *devType = StructType::getTypeByName(ctx, "struct.device");
    Value *deviceGEP = nullptr;
    if (embedsStruct(instanceType, devType)) {
      deviceGEP = b.CreateInBoundsGEP(
          instanceType, instance,
          gepIndicesToStruct(instanceType, devType).getValue());
    } else if (embedsStruct(instanceType, devType->getPointerTo())) {
      Value *devicePtrGEP = b.CreateInBoundsGEP(
          instanceType, instance,
          gepIndicesToStruct(instanceType, devType->getPointerTo()).getValue());
      deviceGEP = b.CreateLoad(devicePtrGEP->getType()->getPointerElementType(),
                               devicePtrGEP);
    }
    if (!deviceGEP)
      return;
    devType = cast<StructType>(deviceGEP->getType()->getPointerElementType());
    if (Function *checker = getOrBuildAssertWakeup(m, devType))
      b.CreateCall(checker, deviceGEP);
  }

  Function *genAssertFunction(Module &m, Function *checker,
                              GlobalVariable &storage,
                              GlobalVariable *targetIndex, StringRef suffix) {
    LLVMContext &ctx = m.getContext();
    IntegerType *i64Ty = Type::getInt64Ty(ctx);
    IntegerType *i8Ty = Type::getInt8Ty(ctx);
    StructType *devType = StructType::getTypeByName(ctx, "struct.device");
    Function *rawcountChecker = getOrBuildRawcountChecker(m, i8Ty, 1);
    Function *f =
        Function::Create(FunctionType::get(Type::getVoidTy(ctx), false),
                         GlobalValue::ExternalLinkage,
                         "drvhorn.check_storage." + suffix.str(), &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *check = BasicBlock::Create(ctx, "check", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);
    IRBuilder<> b(entry);

    Value *target = b.CreateLoad(i64Ty, targetIndex);
    Value *cond = b.CreateICmpNE(target, ConstantInt::get(i64Ty, -1));
    b.CreateCondBr(cond, check, ret);

    b.SetInsertPoint(check);
    StructType *krefType = cast<StructType>(
        checker->getArg(0)->getType()->getPointerElementType());
    ArrayType *storageType = cast<ArrayType>(storage.getValueType());
    Value *elem = b.CreateInBoundsGEP(storageType, &storage,
                                      {ConstantInt::get(i64Ty, 0), target});
    StructType *elemType;
    if (storageType->getElementType()->isPointerTy()) {
      elemType = cast<StructType>(
          storageType->getElementType()->getPointerElementType());
      elem = b.CreateLoad(storageType->getElementType(), elem);
    } else {
      elemType = cast<StructType>(storageType->getElementType());
    }
    if (embedsStruct(elemType, krefType)) {
      Value *krefPtr = b.CreateInBoundsGEP(
          elemType, elem, gepIndicesToStruct(elemType, krefType).getValue());
      b.CreateCall(checker, krefPtr);
    } else {
      // If it does not embed kref, it should be struct.fwnode_handle
      Value *countGEP = b.CreateInBoundsGEP(
          elemType, elem, {b.getInt64(0), b.getInt32(FWNODE_REFCOUNT_INDEX)});
      LoadInst *count = b.CreateLoad(i8Ty, countGEP);
      b.CreateCall(rawcountChecker, count);
    }
    if (equivTypes(elemType, devType)) {
      if (Function *checker = getOrBuildAssertWakeup(m, elemType))
        b.CreateCall(checker, elem);
    }
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRetVoid();
    return f;
  }

  Function *getOrBuildAssertWakeup(Module &m, StructType *devType) {
    if (!m.getFunction("drvhorn.device_wakeup_enable"))
      return nullptr;
    std::string name = "drvhorn.assert_wakeup." + devType->getName().str();
    if (Function *f = m.getFunction(name))
      return f;
    LLVMContext &ctx = m.getContext();
    IntegerType *i16Ty = Type::getInt16Ty(ctx);
    Function *f = Function::Create(
        FunctionType::get(Type::getVoidTy(ctx), devType->getPointerTo(), false),
        GlobalValue::PrivateLinkage, name, &m);
    Function *rawcountChecker = getOrBuildRawcountChecker(m, i16Ty, 0);
    StructType *devPmInfoType =
        StructType::getTypeByName(ctx, "struct.dev_pm_info");
    Value *dev = f->getArg(0);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);

    IRBuilder<> b(entry);
    Value *pmInfoGEP = b.CreateInBoundsGEP(
        devType, dev, gepIndicesToStruct(devType, devPmInfoType).getValue());
    Value *wakeupGEP = b.CreateInBoundsGEP(
        pmInfoGEP->getType()->getPointerElementType(), pmInfoGEP,
        {b.getInt64(0), b.getInt32(DEVPMINFO_WAKEUP_INDEX)});
    LoadInst *wakeup = b.CreateLoad(i16Ty, wakeupGEP);
    b.CreateCall(rawcountChecker, wakeup);
    b.CreateRetVoid();
    return f;
  }
};

char AssertKrefs::ID = 0;

Pass *createAssertKrefsPass() { return new AssertKrefs(); }
} // namespace seahorn
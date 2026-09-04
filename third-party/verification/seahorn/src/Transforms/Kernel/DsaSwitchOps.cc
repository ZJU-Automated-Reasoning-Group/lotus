#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Transforms/Kernel/SetupEntrypoint.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

#define DSA_SWITCH_OPS_SETUP_INDEX 4u

class DsaSwitchOps : public ModulePass {
public:
  static char ID;

  DsaSwitchOps(StringRef name) : ModulePass(ID), name(name) {}

  bool runOnModule(Module &m) override {
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(ctx);
    Function *setup = getSetupFn(m);
    if (!setup) {
      errs() << "No probe function found for " << name << "\n";
      return false;
    }
    Function *main = Function::Create(
        FunctionType::get(i32Ty, false),
        GlobalValue::LinkageTypes::ExternalLinkage, "main", &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", main);
    BasicBlock *fail = BasicBlock::Create(ctx, "fail", main);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", main);

    Value *instance = buildEntryBlock(m, setup, entry, fail, ret);
    buildFailBlock(m, fail, ret, instance);
    buildRetBlock(m, ret);

    return true;
  }

  virtual StringRef getPassName() const override { return "DsaSwitchOps"; }

private:
  StringRef name;
  DenseMap<const Type *, Function *> ndfn;

  Function *getSetupFn(Module &m) {
    GlobalVariable *drv = m.getGlobalVariable(name, true);
    Constant *setup =
        drv->getInitializer()->getAggregateElement(DSA_SWITCH_OPS_SETUP_INDEX);
    return dyn_cast_or_null<Function>(setup);
  }

  Value *buildEntryBlock(Module &m, Function *setup, BasicBlock *entry,
                         BasicBlock *fail, BasicBlock *ret) {
    IRBuilder<> b(entry);
    StructType *dsaSwitchType =
        cast<StructType>(setup->getArg(0)->getType()->getPointerElementType());
    Value *dsaSwitch = allocType(m, b, dsaSwitchType);
    setupDsaSwitch(m, b, dsaSwitch, dsaSwitchType);
    CallInst *call = b.CreateCall(setup, dsaSwitch);
    Value *isZero = b.CreateICmpEQ(call, ConstantInt::get(call->getType(), 0));
    b.CreateCondBr(isZero, ret, fail);
    return dsaSwitch;
  }

  void setupDsaSwitch(Module &m, IRBuilder<> &b, Value *dsaSwitch,
                      StructType *dsaSwitchType) {
    Type *deviceType =
        dsaSwitchType->getElementType(0)->getPointerElementType();
    Value *devPtr = b.CreateAlloca(deviceType);
    setupDevicePtr(m, b, devPtr);
    Value *gep =
        b.CreateGEP(dsaSwitchType, dsaSwitch, {b.getInt64(0), b.getInt32(0)});
    b.CreateStore(devPtr, gep);
  }

  Value *allocType(Module &m, IRBuilder<> &b, Type *type) {
    AllocaInst *alloc = b.CreateAlloca(type);
    Function *nondet = getNondetFn(type, m);
    CallInst *val = b.CreateCall(nondet);
    // Fill the allocated memory with a nondet value.
    b.CreateStore(val, alloc);
    return alloc;
  }

  Function *getNondetFn(Type *type, Module &m) {
    auto it = ndfn.find(type);
    if (it != ndfn.end()) {
      return it->second;
    }
    Function *res =
        createNewNondetFn(m, *type, ndfn.size(), "verifier.nondet.");
    ndfn[type] = res;
    return res;
  }

  Function *createNewNondetFn(Module &m, Type &type, unsigned num,
                              std::string prefix) {
    std::string name;
    unsigned c = num;
    do {
      name = prefix + std::to_string(c++);
    } while (m.getNamedValue(name));
    return dyn_cast<Function>(m.getOrInsertFunction(name, &type).getCallee());
  }
};

char DsaSwitchOps::ID = 0;

Pass *createDsaSwitchOpsPass(StringRef name) { return new DsaSwitchOps(name); }

} // namespace seahorn
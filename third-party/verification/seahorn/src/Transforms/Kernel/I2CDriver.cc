#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Transforms/Kernel/SetupEntrypoint.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

#define I2C_PROBE_INDEX 1

class I2CDriver : public ModulePass {
public:
  static char ID;

  I2CDriver(StringRef name) : ModulePass(ID), name(name) {}

  bool runOnModule(Module &m) override {
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(ctx);
    Function *probe = getProbeFn(m);
    if (!probe) {
      errs() << "No probe function found for " << name << "\n";
      return false;
    }
    Function *main = Function::Create(
        FunctionType::get(i32Ty, false),
        GlobalValue::LinkageTypes::ExternalLinkage, "main", &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", main);
    BasicBlock *fail = BasicBlock::Create(ctx, "fail", main);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", main);

    Value *instance = buildEntryBlock(m, probe, entry, fail, ret);
    buildFailBlock(m, fail, ret, instance);
    buildRetBlock(m, ret);

    return true;
  }

  virtual StringRef getPassName() const override { return "I2CDriver"; }

private:
  StringRef name;

  Function *getProbeFn(Module &m) {
    GlobalVariable *drv = m.getGlobalVariable(name, true);
    Constant *probe =
        drv->getInitializer()->getAggregateElement(I2C_PROBE_INDEX);
    return dyn_cast_or_null<Function>(probe);
  }

  Value *buildEntryBlock(Module &m, Function *probe, BasicBlock *entry,
                         BasicBlock *fail, BasicBlock *ret) {
    IRBuilder<> b(entry);
    StructType *i2cClientType =
        cast<StructType>(probe->getArg(0)->getType()->getPointerElementType());
    Value *i2cClient = setupI2CClient(m, b, i2cClientType);
    CallInst *call = b.CreateCall(probe, i2cClient);
    Value *isZero = b.CreateICmpEQ(call, ConstantInt::get(call->getType(), 0));
    b.CreateCondBr(isZero, ret, fail);
    return i2cClient;
  }

  Value *setupI2CClient(Module &m, IRBuilder<> &b, StructType *i2cClientType) {
    LLVMContext &ctx = m.getContext();
    StructType *deviceType = StructType::getTypeByName(ctx, "struct.device");
    AllocaInst *i2cClient = b.CreateAlloca(i2cClientType);
    Value *devicePtr = b.CreateInBoundsGEP(
        i2cClientType, i2cClient,
        gepIndicesToStruct(i2cClientType, deviceType).getValue());
    setupDevicePtr(m, b, devicePtr);
    return i2cClient;
  }
};

char I2CDriver::ID = 0;

Pass *createI2CDriverPass(StringRef i2cDriverName) {
  return new I2CDriver(i2cDriverName);
}

} // namespace seahorn
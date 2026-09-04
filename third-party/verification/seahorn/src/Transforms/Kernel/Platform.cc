#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Transforms/Kernel/SetupEntrypoint.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

#define PDEV_PROBE_INDEX 0u
#define PDEV_DEVICE_GEP_INDEX 3

class PlatformDriver : public ModulePass {
public:
  static char ID;

  PlatformDriver(StringRef name) : ModulePass(ID), name(name) {}

  bool runOnModule(Module &m) override {
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(ctx);
    Function *probe = getProbeFn(m);
    if (!probe) {
      errs() << "No probe function found for " << name << "\n";
      return false;
    }
    Function *main = Function::Create(FunctionType::get(i32Ty, false),
                                      GlobalValue::ExternalLinkage, "main", &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", main);
    BasicBlock *fail = BasicBlock::Create(ctx, "fail", main);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", main);

    Value *instance = buildEntryBlock(m, probe, entry, fail, ret);
    buildFailBlock(m, fail, ret, instance);
    buildRetBlock(m, ret);

    return true;
  }

  virtual StringRef getPassName() const override { return "PlatformDriver"; }

private:
  StringRef name;
  DenseMap<const Type *, Function *> ndfn;

  Function *getProbeFn(Module &m) {
    GlobalVariable *drv = m.getGlobalVariable(name, true);
    Constant *probe =
        drv->getInitializer()->getAggregateElement(PDEV_PROBE_INDEX);
    return dyn_cast_or_null<Function>(probe);
  }

  Value *buildEntryBlock(Module &m, Function *probe, BasicBlock *entry,
                         BasicBlock *fail, BasicBlock *ret) {
    IRBuilder<> b(entry);
    Type *pdevType = probe->getArg(0)->getType()->getPointerElementType();
    Value *pdev = allocType(m, b, pdevType);
    setupPDev(m, b, pdev);
    CallInst *call = b.CreateCall(probe, pdev);
    Value *zero = b.CreateICmpEQ(call, ConstantInt::get(call->getType(), 0));
    b.CreateCondBr(zero, ret, fail);
    return pdev;
  }

  void setupPDev(Module &m, IRBuilder<> &b, Value *pdev) {
    Value *devPtr = b.CreateInBoundsGEP(
        pdev->getType()->getPointerElementType(), pdev,
        {b.getInt64(0), b.getInt32(PDEV_DEVICE_GEP_INDEX)}, "device");
    setupDevicePtr(m, b, devPtr);
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

char PlatformDriver::ID = 0;

Pass *createPlatformDriverPass(StringRef name) {
  return new PlatformDriver(name);
}
}; // namespace seahorn
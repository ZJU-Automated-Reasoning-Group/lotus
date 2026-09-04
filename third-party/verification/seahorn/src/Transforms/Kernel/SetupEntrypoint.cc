#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {
#define DEVICE_DRVDATA_INDEX 8

void setupDevicePtr(Module &m, IRBuilder<> &b, Value *devPtr) {
  LLVMContext &ctx = m.getContext();
  Function *krefInit = m.getFunction("drvhorn.kref_init");
  StructType *krefType =
      StructType::getTypeByName(m.getContext(), "struct.kref");
  StructType *devType =
      cast<StructType>(devPtr->getType()->getPointerElementType());
  StructType *devPmInfoType =
      StructType::getTypeByName(ctx, "struct.dev_pm_info");
  IntegerType *i8Ty = Type::getInt8Ty(ctx);

  // disable wakeup
  SmallVector<Value *> indices(
      gepIndicesToStruct(devType, devPmInfoType).getValue());
  indices.push_back(b.getInt32(DEVPMINFO_WAKEUP_INDEX));
  Value *wakeupGEP = b.CreateInBoundsGEP(devType, devPtr, indices);
  b.CreateStore(b.getInt16(0), wakeupGEP);
  Value *krefGEP = b.CreateInBoundsGEP(
      devType, devPtr, gepIndicesToStruct(devType, krefType).getValue());
  b.CreateCall(krefInit, krefGEP);

  // setup driver_data
  Value *driverDataPtr = b.CreateInBoundsGEP(
      devType, devPtr, {b.getInt64(0), b.getInt32(DEVICE_DRVDATA_INDEX)},
      "driver_data");
  AllocaInst *driverData = b.CreateAlloca(i8Ty, b.getInt64(0x1000));
  b.CreateStore(driverData, driverDataPtr);

  // setup of_node
  // TODO: this causes false positives
  const SmallVector<Value *> &devNodeIndices =
      gepIndicesToStruct(
          devType,
          StructType::getTypeByName(ctx, "struct.device_node")->getPointerTo())
          .getValue();
  Value *ofNodeGEP =
      b.CreateInBoundsGEP(devType, devPtr, devNodeIndices, "of_node");
  // Function *devNodeGetter = m.getFunction("drvhorn.gen.devnode");
  // Value *ofNode = b.CreateCall(devNodeGetter);
  // if (ofNode->getType() != ofNodeGEP->getType()->getPointerElementType())
  //   ofNode =
  //       b.CreateBitCast(ofNode,
  //       ofNodeGEP->getType()->getPointerElementType());
  Value *ofNode = b.CreateAlloca(
      ofNodeGEP->getType()->getPointerElementType()->getPointerElementType());
  b.CreateStore(ofNode, ofNodeGEP);
}

void buildFailBlock(Module &m, BasicBlock *fail, BasicBlock *ret,
                    Value *instance) {
  IRBuilder<> b(fail);
  LLVMContext &ctx = m.getContext();
  Type *voidTy = Type::getVoidTy(ctx);
  FunctionType *fnType;
  if (instance)
    fnType = FunctionType::get(voidTy, instance->getType(), false);
  else
    fnType = FunctionType::get(voidTy, false);
  // devresReleaseFn and failFn are filled later in AssertKrefs.cc
  Function *failFn = Function::Create(fnType, GlobalValue::ExternalLinkage,
                                      "drvhorn.fail", &m);
  if (instance)
    b.CreateCall(failFn, instance);
  else
    b.CreateCall(failFn);
  b.CreateBr(ret);
}

void buildRetBlock(Module &m, BasicBlock *ret) {
  IRBuilder<> b(ret);
  Type *i32Ty = Type::getInt32Ty(m.getContext());
  b.CreateRet(ConstantInt::get(i32Ty, 0));
}
}; // namespace seahorn
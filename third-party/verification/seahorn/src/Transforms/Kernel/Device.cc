#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Pass.h"

#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

struct ContainerOfVisitor : InstVisitor<ContainerOfVisitor> {
public:
  ContainerOfVisitor(Value *gep, Value *containerOfReplacer)
      : gep(gep), containerOfReplacer(containerOfReplacer) {
    nullContainerOf = Constant::getNullValue(containerOfReplacer->getType());
  }

  void visitICmpInst(ICmpInst &icmp) {
    Value *lhs = icmp.getOperand(0);
    Value *rhs = icmp.getOperand(1);
    if (lhs == gep) {
      lhs = containerOfReplacer;
    } else if (isa<ConstantPointerNull>(lhs)) {
      lhs = nullContainerOf;
    } else {
      errs() << "ContainerOfVisitor: invalid operand" << icmp << "\n";
      std::exit(1);
    }
    if (rhs == gep) {
      rhs = containerOfReplacer;
    } else if (isa<ConstantPointerNull>(rhs)) {
      rhs = nullContainerOf;
    } else {
      errs() << "ContainerOfVisitor: invalid operand" << icmp << "\n";
      std::exit(1);
    }
    ICmpInst *newICmp = new ICmpInst(&icmp, icmp.getPredicate(), lhs, rhs, "");
    icmp.replaceAllUsesWith(newICmp);
    icmp.eraseFromParent();
  }

  void visitBitCastInst(BitCastInst &bitcast) {
    if (bitcast.getDestTy() != containerOfReplacer->getType()) {
      bitcast.replaceUsesOfWith(gep, containerOfReplacer);
    } else {
      bitcast.replaceAllUsesWith(containerOfReplacer);
      bitcast.eraseFromParent();
    }
  }

  void visitStoreInst(StoreInst &store) {
    if (store.getValueOperand() == gep) {
      store.setOperand(0, containerOfReplacer);
      Value *ptr = store.getPointerOperand();
      BitCastInst *bitcast = new BitCastInst(
          ptr, containerOfReplacer->getType()->getPointerTo(), "", &store);
      store.setOperand(1, bitcast);
    }
  }

  void visitInstruction(Instruction &inst) {
    inst.replaceUsesOfWith(gep, containerOfReplacer);
  }

private:
  Value *gep;
  Value *containerOfReplacer;
  Value *nullContainerOf;
};

struct DeviceGEP {
  StructType *devType;
  uint64_t index;
};

struct DeviceGEPGetter : InstVisitor<DeviceGEPGetter, Optional<DeviceGEP>> {
public:
  DeviceGEPGetter() {}

  Optional<DeviceGEP> getGEPIndices(CallInst *allocCall) {
    visited.clear();
    visited.insert(allocCall);
    prev = allocCall;
    for (User *user : allocCall->users()) {
      if (Optional<DeviceGEP> res = visitUser(user))
        return res;
    }
    return None;
  }

  Optional<DeviceGEP> visitGetElementPtrInst(GetElementPtrInst &gep) {
    for (User *user : gep.users()) {
      if (Optional<DeviceGEP> res = visitUser(user)) {
        if (gep.getNumIndices() > 1)
          errs() << "DeviceGEPGetter: unexpected number of indices in "
                 << gep.getFunction()->getName() << ' ' << gep << "\n";
        ConstantInt *cidx = dyn_cast<ConstantInt>(gep.idx_begin());
        if (!cidx)
          return None;
        uint64_t idx = cidx->getZExtValue();
        return DeviceGEP{res->devType, res->index + idx};
      }
    }
    return None;
  }

  Optional<DeviceGEP> visitBitCastInst(BitCastInst &bitcast) {
    if (StructType *destTy = dyn_cast<StructType>(
            bitcast.getDestTy()->getPointerElementType())) {
      StructType *deviceType =
          StructType::getTypeByName(destTy->getContext(), "struct.device");
      if (embedsStruct(destTy, deviceType)) {
        return DeviceGEP{destTy, 0};
      }
    }
    return None;
  }

  Optional<DeviceGEP> visitCallInst(CallInst &call) {
    Function *f = extractCalledFunction(call);
    if (!f)
      return None;
    User *argUser = prev;
    for (unsigned i = 0; i < call.arg_size(); i++) {
      if (call.getArgOperand(i) == argUser) {
        if (Optional<DeviceGEP> res = visitArg(f->getArg(i)))
          return res;
        break;
      }
    }
    return None;
  }

  Optional<DeviceGEP> visitInstruction(Instruction &inst) { return None; }

private:
  DenseSet<const User *> visited;
  User *prev;

  Optional<DeviceGEP> visitArg(Argument *arg) {
    for (User *user : arg->users()) {
      if (Optional<DeviceGEP> res = visitUser(user))
        return res;
    }
    return None;
  }

  Optional<DeviceGEP> visitUser(User *user) {
    if (!visited.insert(user).second)
      return None;
    prev = user;
    if (Instruction *inst = dyn_cast<Instruction>(user)) {
      return visit(inst);
    } else {
      for (User *u : user->users()) {
        if (Optional<DeviceGEP> res = visitUser(u))
          return res;
      }
      return None;
    }
  }
};

#define STORAGE_SIZE 256

class HandleDevices : public ModulePass {
public:
  static char ID;

  HandleDevices() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    Function *updateIndex = buildUpdateIndex(m);
    LLVMContext &ctx = m.getContext();
    Attribute fwnodeAttr = Attribute::get(ctx, "drvhorn.fwnode");
    Attribute checkPointAttr = Attribute::get(ctx, "drvhorn.checkpoint");

    Function *devNodeGetter =
        handleDeviceNodeFinders(m, updateIndex, checkPointAttr);
    handleFwnodeGet(m, fwnodeAttr);
    Function *fwnodePutter = handleFwnodePut(m, fwnodeAttr, checkPointAttr);
    handleFwnodeFinders(m, fwnodePutter, updateIndex, fwnodeAttr,
                        checkPointAttr);
    handleDeviceFinders(m, updateIndex, checkPointAttr);
    Function *devInit = handleDeviceInitialize(m, checkPointAttr);
    ignoreSomeFunctions(m);
    handleDeviceLink(m);
    handleDeviceAllocation(m, devInit, updateIndex, checkPointAttr);
    handleDevmFunctions(m, checkPointAttr);
    handleCDevDeviceAdd(m);
    handleCDevDeviceDel(m);
    handleDeviceWakeupEnable(m, checkPointAttr);
    handleDeviceWakeupDisable(m, checkPointAttr);

    handleOfParsePhandleWithArgs(m, devNodeGetter);
    handleOfPhandleIteratorNext(m, devNodeGetter);
    // TODO: handle of_clk_del_provider?

    stubFwnodeConnectionFindMatch(m);
    stubFwnodeConnectionFindMatches(m);

    handleCpufreqGet(m, updateIndex, checkPointAttr);
    handleRegulatorGet(m, updateIndex, checkPointAttr);
    handleLedRegister(m, updateIndex, checkPointAttr);
    return true;
  }

  virtual StringRef getPassName() const override { return "HandleDevices"; }

  void getAnalysisUsage(AnalysisUsage &au) const override {
    au.addRequired<SeaBuiltinsInfoWrapperPass>();
    au.setPreservesAll();
  }

private:
  Function *buildUpdateIndex(Module &m) {
    LLVMContext &ctx = m.getContext();
    IntegerType *i64Ty = Type::getInt64Ty(ctx);
    Type *voidTy = Type::getVoidTy(ctx);
    FunctionType *ft =
        FunctionType::get(voidTy, {i64Ty, i64Ty->getPointerTo()}, false);
    Function *f = Function::Create(ft, GlobalVariable::InternalLinkage,
                                   "drvhorn.update_index", m);
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Value *ndCond = b.CreateCall(ndBool);
    b.CreateCondBr(ndCond, body, ret);

    b.SetInsertPoint(body);
    b.CreateStore(f->getArg(0), f->getArg(1));
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRetVoid();
    return f;
  }

  struct StorageGlobals {
    GlobalVariable *storage;
    GlobalVariable *curIndex;
    GlobalVariable *targetIndex;
  };

  // returns the device node generator function
  Function *handleDeviceNodeFinders(Module &m, Function *updateIndex,
                                    Attribute checkPointAttr) {
    struct FinderInfo {
      StringRef name;
      Optional<size_t> putNodeIndex;
      Optional<size_t> returnIfNullArgIndex;
    };
    FinderInfo finders[] = {
        // drivers/of/base.c
        {"of_find_all_nodes", 0, None},
        {"of_get_parent", None, 0},
        {"of_get_next_parent", 0, 0},
        {"of_get_next_child", 1, 0},
        {"of_get_next_available_child", 1, 0},
        {"of_get_next_cpu_node", 0, None},
        {"of_get_compatible_child", None, 0},
        {"of_get_child_by_name", None, 0},
        {"of_find_node_opts_by_path", None, None},
        {"of_find_node_by_name", 0, None},
        {"of_find_node_by_type", 0, None},
        {"of_find_compatible_node", 0, None},
        {"of_find_node_with_property", 0, None},
        {"of_find_matching_node_and_match", 0, None},
        {"of_find_node_by_phandle", None, None},
        {"of_find_next_cache_node", None, None},
        // drivers/of/irq.c
        {"of_irq_find_parent", None, 0},
        // drivers/of/property.c
        {"of_graph_get_port_by_id", None, None},
        {"of_graph_get_next_endpoint", 1, None},
        {"of_graph_get_next_endpoint", 1, 0},
        {"of_graph_get_endpoint_by_regs", None, None},
        {"of_graph_get_port_parent", None, 0},
        {"of_graph_get_remote_port_parent", None, None},
        {"of_graph_get_remote_port", None, None},
        {"of_graph_get_remote_node", None, None},
        // drivers/of/cpu.c
        {"of_get_cpu_node", None, None},
        {"of_cpu_device_node_get", None, None},
        {"of_get_cpu_state_node", None, None},
        // drivers/regulator/core.c
        {"of_get_regulator", None, None},
        {"of_get_child_regulator", None, None},
    };

    LLVMContext &ctx = m.getContext();
    Constant *ofNodeGet = m.getFunction("of_node_get");
    Constant *ofNodePut = m.getFunction("of_node_put");
    Function *devNodeGetter = nullptr;
    for (const FinderInfo &info : finders) {
      Function *f = m.getFunction(info.name);
      if (!f)
        continue;
      f->deleteBody();
      f->setName("drvhorn." + info.name);

      StructType *devNodeType =
          cast<StructType>(f->getReturnType()->getPointerElementType());
      if (!devNodeGetter) {
        devNodeGetter = buildStorageElemGenerator(m, devNodeType, updateIndex,
                                                  {}, checkPointAttr);
        devNodeGetter->setName("drvhorn.gen.devnode");
      }
      BasicBlock *earlyReturn = info.returnIfNullArgIndex.hasValue()
                                    ? BasicBlock::Create(ctx, "early_return", f)
                                    : nullptr;
      BasicBlock *body = BasicBlock::Create(ctx, "body", f);
      BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

      IRBuilder<> b(ctx);
      if (earlyReturn) {
        b.SetInsertPoint(earlyReturn);
        Value *cond = b.CreateIsNull(f->getArg(*info.returnIfNullArgIndex));
        b.CreateCondBr(cond, ret, body);
      }

      b.SetInsertPoint(body);
      Value *devNode = b.CreateCall(devNodeGetter);
      if (devNode->getType() != devNodeType->getPointerTo())
        devNode = b.CreateBitCast(devNode, devNodeType->getPointerTo());
      FunctionType *ofNodeGetType = FunctionType::get(
          devNodeType->getPointerTo(), devNodeType->getPointerTo(), false);
      if (ofNodeGet->getType() != ofNodeGetType->getPointerTo())
        ofNodeGet =
            ConstantExpr::getBitCast(ofNodeGet, ofNodeGetType->getPointerTo());
      b.CreateCall(ofNodeGetType, ofNodeGet, devNode);
      b.CreateBr(ret);

      b.SetInsertPoint(ret);
      PHINode *retPhi = b.CreatePHI(devNodeType->getPointerTo(), 2);
      retPhi->addIncoming(devNode, body);
      if (earlyReturn) {
        Constant *null = ConstantPointerNull::get(devNodeType->getPointerTo());
        retPhi->addIncoming(null, earlyReturn);
      }
      if (info.putNodeIndex.hasValue()) {
        Value *from = f->getArg(info.putNodeIndex.getValue());
        FunctionType *ofNodePutType = FunctionType::get(
            Type::getVoidTy(ctx), devNodeType->getPointerTo(), false);
        if (ofNodePut->getType() != ofNodePutType->getPointerTo())
          ofNodePut = ConstantExpr::getBitCast(ofNodePut,
                                               ofNodePutType->getPointerTo());
        b.CreateCall(ofNodePutType, ofNodePut, from);
      }
      b.CreateRet(retPhi);
    }
    return devNodeGetter;
  }

  void handleFwnodeGet(Module &m, Attribute attr) {
    Function *f = m.getFunction("fwnode_handle_get");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.fwnode_get");
    f->addFnAttr(attr);

    Argument *fwnode = f->getArg(0);
    LLVMContext &ctx = m.getContext();
    BasicBlock *blk = BasicBlock::Create(ctx, "blk", f);
    IntegerType *i8Type = Type::getInt8Ty(ctx);

    IRBuilder<> b(blk);
    Value *countGEP =
        b.CreateInBoundsGEP(fwnode->getType()->getPointerElementType(), fwnode,
                            {b.getInt64(0), b.getInt32(FWNODE_REFCOUNT_INDEX)});
    LoadInst *count = b.CreateLoad(i8Type, countGEP);
    Value *newCount = b.CreateAdd(count, b.getInt8(1));
    b.CreateStore(newCount, countGEP);
    b.CreateRet(fwnode);
  }

  Function *handleFwnodePut(Module &m, Attribute fwnodeAttr,
                            Attribute checkPointAttr) {
    Function *f = m.getFunction("fwnode_handle_put");
    if (!f)
      return nullptr;
    f->deleteBody();
    f->setName("drvhorn.fwnode_put");
    f->addFnAttr(fwnodeAttr);
    f->addFnAttr(checkPointAttr);

    Argument *fwnode = f->getArg(0);
    LLVMContext &ctx = m.getContext();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);
    IntegerType *i8Type = Type::getInt8Ty(ctx);

    IRBuilder<> b(entry);
    Value *isNull = b.CreateIsNull(fwnode);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    Value *countGEP =
        b.CreateInBoundsGEP(fwnode->getType()->getPointerElementType(), fwnode,
                            {b.getInt64(0), b.getInt32(FWNODE_REFCOUNT_INDEX)});
    LoadInst *count = b.CreateLoad(i8Type, countGEP);
    Value *newCount = b.CreateSub(count, b.getInt8(1));
    b.CreateStore(newCount, countGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRetVoid();
    return f;
  }

  void handleFwnodeFinders(Module &m, Function *fwnodePutter,
                           Function *updateIndex, Attribute fwnodeAttr,
                           Attribute checkPointAttr) {
    struct FinderInfo {
      StringRef name;
      Optional<unsigned> putIndex;
    };

    FinderInfo finders[] = {
        {"fwnode_find_reference", None},
        {"fwnode_get_parent", None},
        {"fwnode_get_next_parent", 0},
        {"fwnode_get_nth_parent", 0},
        {"fwnode_get_next_child_node", 1},
        {"fwnode_get_next_available_child_node", 1},
        {"device_get_next_child_node", 1},
        {"fwnode_get_named_child_node", None},
        {"device_get_named_child_node", None},
        {"fwnode_graph_get_next_endpoint", 1},
        {"fwnode_graph_get_port_parent", None},
        {"fwnode_graph_get_remote_port_parent", None},
        {"fwnode_graph_get_remote_port", None},
        {"fwnode_graph_get_remote_endpoint", None},
        {"fwnode_graph_get_endpoint_by_id", None},
    };
    LLVMContext &ctx = m.getContext();
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    IntegerType *i64Ty = Type::getInt64Ty(ctx);

    for (const FinderInfo &info : finders) {
      Function *f = m.getFunction(info.name);
      if (!f)
        continue;
      f->deleteBody();
      f->setName("drvhorn.fwnode_getter." + info.name);
      f->addFnAttr(checkPointAttr);
      f->addFnAttr(fwnodeAttr);

      BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
      BasicBlock *body = BasicBlock::Create(ctx, "body", f);
      BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

      StructType *fwnodeType =
          cast<StructType>(f->getReturnType()->getPointerElementType());
      StorageGlobals globals =
          getStorageAndIndex(m, fwnodeType, fwnodeType->getName().str());
      GlobalVariable *storage = globals.storage;
      GlobalVariable *index = globals.curIndex;
      GlobalVariable *targetIndex = globals.targetIndex;

      IRBuilder<> b(entry);
      if (info.putIndex.hasValue()) {
        Value *prev = f->getArg(*info.putIndex);
        if (fwnodePutter->getArg(0)->getType() != prev->getType())
          prev = b.CreateBitCast(prev, fwnodePutter->getArg(0)->getType());
        b.CreateCall(fwnodePutter, prev);
      }
      CallInst *ndCond = b.CreateCall(ndBool);
      LoadInst *curIndex = b.CreateLoad(i64Ty, index);
      Value *withinRange = b.CreateICmpULT(curIndex, b.getInt64(STORAGE_SIZE));
      Value *cond = b.CreateAnd(ndCond, withinRange);
      b.CreateCondBr(cond, body, ret);

      b.SetInsertPoint(body);
      Value *fwnode = b.CreateInBoundsGEP(storage->getValueType(), storage,
                                          {b.getInt64(0), curIndex});
      Value *countGEP = b.CreateInBoundsGEP(
          fwnode->getType()->getPointerElementType(), fwnode,
          {b.getInt64(0), b.getInt32(FWNODE_REFCOUNT_INDEX)});
      b.CreateStore(b.getInt8(2), countGEP);
      Value *nxtIndex = b.CreateAdd(curIndex, b.getInt64(1));
      b.CreateStore(nxtIndex, index);
      b.CreateCall(updateIndex, {curIndex, targetIndex});
      b.CreateBr(ret);

      b.SetInsertPoint(ret);
      PHINode *retPhi = b.CreatePHI(fwnode->getType(), 2);
      Constant *null = Constant::getNullValue(fwnode->getType());
      retPhi->addIncoming(null, entry);
      retPhi->addIncoming(fwnode, body);
      b.CreateRet(retPhi);
    }
  }

  StorageGlobals getStorageAndIndex(Module &m, StructType *elemType,
                                    std::string suffix) {
    std::string storageName = "drvhorn.storage." + suffix;
    std::string indexName = "drvhorn.index." + suffix;
    std::string targetIndexName = "drvhorn.target_index." + suffix;
    GlobalVariable *storage = m.getGlobalVariable(storageName, true);
    GlobalVariable *index = m.getGlobalVariable(indexName, true);
    GlobalVariable *targetIndex = m.getGlobalVariable(targetIndexName, true);
    if (!storage) {
      ArrayType *storageType = ArrayType::get(elemType, STORAGE_SIZE);
      storage =
          new GlobalVariable(m, storageType, false, GlobalValue::PrivateLinkage,
                             Constant::getNullValue(storageType), storageName);
      IntegerType *i64Ty = Type::getInt64Ty(m.getContext());
      index = new GlobalVariable(m, i64Ty, false, GlobalValue::PrivateLinkage,
                                 ConstantInt::get(i64Ty, 0), indexName);
      targetIndex =
          new GlobalVariable(m, i64Ty, false, GlobalValue::PrivateLinkage,
                             ConstantInt::get(i64Ty, -1), targetIndexName);
    }
    return {storage, index, targetIndex};
  }

  Function *
  buildStorageElemGenerator(Module &m, StructType *elemType,
                            Function *updateIndex,
                            const SmallVector<Value *> &embeddedIndices,
                            Optional<Attribute> attr = None) {
    StructType *resType;
    if (embeddedIndices.empty())
      resType = elemType;
    else
      resType = cast<StructType>(getGEPType(elemType, embeddedIndices));
    std::string name = "drvhorn.gen." + resType->getName().str();
    if (Function *f = m.getFunction(name))
      return f;
    Function *f =
        Function::Create(FunctionType::get(resType->getPointerTo(), false),
                         GlobalValue::PrivateLinkage, name, &m);
    StorageGlobals globals =
        getStorageAndIndex(m, elemType, elemType->getName().str());
    GlobalVariable *storage = globals.storage;
    GlobalVariable *index = globals.curIndex;
    GlobalVariable *targetIndex = globals.targetIndex;

    Function *ndBool = getOrCreateNdIntFn(m, 1);
    Function *krefInit = m.getFunction("drvhorn.kref_init");
    StructType *krefType = cast<StructType>(
        krefInit->getArg(0)->getType()->getPointerElementType());
    LLVMContext &ctx = m.getContext();
    StructType *devType = StructType::getTypeByName(ctx, "struct.device");
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);
    IntegerType *i64Ty = Type::getInt64Ty(ctx);

    IRBuilder<> b(entry);
    CallInst *ndCond = b.CreateCall(ndBool);
    LoadInst *curIndex = b.CreateLoad(i64Ty, index);
    Value *withinRange = b.CreateICmpULT(curIndex, b.getInt64(STORAGE_SIZE));
    Value *cond = b.CreateAnd(ndCond, withinRange);
    b.CreateCondBr(cond, body, ret);

    b.SetInsertPoint(body);
    Value *elem = b.CreateInBoundsGEP(storage->getValueType(), storage,
                                      {b.getInt64(0), curIndex});
    if (!embeddedIndices.empty()) {
      elem = b.CreateInBoundsGEP(elemType, elem, embeddedIndices);
      elemType = cast<StructType>(elem->getType()->getPointerElementType());
    }
    Value *krefPtr = b.CreateInBoundsGEP(
        elemType, elem, gepIndicesToStruct(elemType, krefType).getValue());
    b.CreateCall(krefInit, krefPtr);
    Value *nxtIndex = b.CreateAdd(curIndex, b.getInt64(1));
    if (equivTypes(elemType, devType)) {
      StructType *devPmInfoType =
          StructType::getTypeByName(ctx, "struct.dev_pm_info");
      Value *pmInfoGEP = b.CreateInBoundsGEP(
          elemType, elem,
          gepIndicesToStruct(elemType, devPmInfoType).getValue());
      Value *wakeupGEP = b.CreateInBoundsGEP(
          pmInfoGEP->getType()->getPointerElementType(), pmInfoGEP,
          {b.getInt64(0), b.getInt32(DEVPMINFO_WAKEUP_INDEX)});
      b.CreateStore(b.getInt16(0), wakeupGEP);
    }
    b.CreateStore(nxtIndex, index);
    b.CreateCall(updateIndex, {curIndex, targetIndex});
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    PHINode *retPhi = b.CreatePHI(resType->getPointerTo(), 2);
    Constant *null = ConstantPointerNull::get(resType->getPointerTo());
    retPhi->addIncoming(null, entry);
    retPhi->addIncoming(elem, body);
    b.CreateRet(retPhi);

    if (attr.hasValue())
      f->addFnAttr(*attr);

    return f;
  }

  void handleDevmFunctions(Module &m, Attribute checkPointAttr) {
    handleDevmAddAction(m, checkPointAttr);

    StringRef names[] = {
        "__devres_alloc_node",
        "devres_add",
    };
    Attribute attr = Attribute::get(m.getContext(), "drvhorn.devm");
    for (StringRef name : names) {
      Function *f = m.getFunction(name);
      if (!f)
        continue;
      f->deleteBody();
      f->setName("drvhorn." + name);
      f->addFnAttr(attr);
    }
  }

  void handleDevmAddAction(Module &m, Attribute checkPointAttr) {
    Function *f = m.getFunction("__devm_add_action");
    if (!f)
      return;
    for (CallInst *call : getCalls(f)) {
      Function *action =
          dyn_cast<Function>(call->getArgOperand(1)->stripPointerCasts());
      if (!action)
        continue;
      Value *data = call->getArgOperand(2);

      IRBuilder<> b(call);
      if (action->getArg(0)->getType() != data->getType())
        data = b.CreateBitCast(data, action->getArg(0)->getType());
      b.CreateCall(action, data);
      action->addFnAttr(checkPointAttr);
      call->replaceAllUsesWith(b.getInt32(0));
      call->eraseFromParent();
    }
  }

  void handleCDevDeviceAdd(Module &m) {
    Function *f = m.getFunction("cdev_device_add");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.cdev_device_add");
    Function *devAdd = m.getFunction("device_add");
    LLVMContext &ctx = m.getContext();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    Value *dev = f->getArg(1);
    IRBuilder<> b(entry);
    if (dev->getType() != devAdd->getArg(0)->getType())
      dev = b.CreateBitCast(dev, devAdd->getArg(0)->getType());
    Value *v = b.CreateCall(devAdd, dev);
    b.CreateRet(v);
  }

  void handleCDevDeviceDel(Module &m) {
    Function *f = m.getFunction("cdev_device_del");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.cdev_device_del");
  }

  void handleDeviceWakeupEnable(Module &m, Attribute checkPointAttr) {
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    Function *enable = m.getFunction("device_wakeup_enable");
    if (!enable)
      return;
    enable->deleteBody();
    enable->setName("drvhorn.device_wakeup_enable");
    enable->addFnAttr(checkPointAttr);
    LLVMContext &ctx = m.getContext();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", enable);
    BasicBlock *body = BasicBlock::Create(ctx, "body", enable);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", enable);
    Value *dev = enable->getArg(0);
    StructType *devType =
        cast<StructType>(dev->getType()->getPointerElementType());
    StructType *devPmInfoType =
        StructType::getTypeByName(ctx, "struct.dev_pm_info");
    IntegerType *i32Ty = Type::getInt32Ty(ctx);

    IRBuilder<> b(entry);
    Value *cond = b.CreateCall(ndBool);
    b.CreateCondBr(cond, body, ret);

    b.SetInsertPoint(body);
    Value *devPmInfoGEP = b.CreateInBoundsGEP(
        devType, dev, gepIndicesToStruct(devType, devPmInfoType).getValue());
    Value *wakeupGEP = b.CreateInBoundsGEP(
        devPmInfoGEP->getType()->getPointerElementType(), devPmInfoGEP,
        {b.getInt64(0), b.getInt32(DEVPMINFO_WAKEUP_INDEX)});
    b.CreateStore(b.getInt16(1), wakeupGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    PHINode *phi = b.CreatePHI(i32Ty, 2);
    phi->addIncoming(ConstantInt::get(i32Ty, -EINVAL), entry);
    phi->addIncoming(ConstantInt::get(i32Ty, 0), body);
    b.CreateRet(phi);
  }

  void handleDeviceWakeupDisable(Module &m, Attribute checkPointAttr) {
    Function *disable = m.getFunction("device_wakeup_disable");
    if (!disable)
      return;
    disable->deleteBody();
    disable->setName("drvhorn.device_wakeup_disable");
    disable->addFnAttr(checkPointAttr);
    LLVMContext &ctx = m.getContext();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", disable);
    BasicBlock *body = BasicBlock::Create(ctx, "body", disable);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", disable);
    Value *dev = disable->getArg(0);
    StructType *devType =
        cast<StructType>(dev->getType()->getPointerElementType());
    StructType *devPmInfoType =
        StructType::getTypeByName(ctx, "struct.dev_pm_info");
    IntegerType *i32Ty = Type::getInt32Ty(ctx);

    IRBuilder<> b(entry);
    Value *isNull = b.CreateIsNull(dev);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    Value *devPmInfoGEP = b.CreateInBoundsGEP(
        devType, dev, gepIndicesToStruct(devType, devPmInfoType).getValue());
    Value *wakeupGEP = b.CreateInBoundsGEP(
        devPmInfoGEP->getType()->getPointerElementType(), devPmInfoGEP,
        {b.getInt64(0), b.getInt32(DEVPMINFO_WAKEUP_INDEX)});
    b.CreateStore(b.getInt16(0), wakeupGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    PHINode *phi = b.CreatePHI(i32Ty, 2);
    phi->addIncoming(ConstantInt::get(i32Ty, -EINVAL), entry);
    phi->addIncoming(ConstantInt::get(i32Ty, 0), body);
    b.CreateRet(phi);
  }

#define DL_FLAG_AUTOREMOVE_SUPPLIER (1 << 4)
#define DL_FLAG_AUTOPROBE_CONSUMER (1 << 5)
  void handleDeviceLink(Module &m) {
    handleDeviceLinkAdd(m);
    handleDeviceLinkDel(m);
    handleDeviceLinkRemove(m);
  }

#define DEVLINK_SUPPLIER_INDEX 0
#define DEVLINK_CONSUMER_INDEX 2
  void handleDeviceLinkAdd(Module &m) {
    Function *f = m.getFunction("device_link_add");
    if (!f)
      return;
    handleDeviceLinkAddAutoremoveCall(m, f);
    f->deleteBody();
    f->setName("drvhorn.device_link_add");

    Argument *consumer = f->getArg(0);
    Argument *supplier = f->getArg(1);

    Type *devlinkType = f->getReturnType()->getPointerElementType();
    LLVMContext &ctx = m.getContext();
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Value *ndCond = b.CreateCall(ndBool);
    b.CreateCondBr(ndCond, body, ret);

    b.SetInsertPoint(body);
    AllocaInst *link = b.CreateAlloca(devlinkType);
    Value *supplierGEP = b.CreateInBoundsGEP(
        devlinkType, link, {b.getInt64(0), b.getInt32(DEVLINK_SUPPLIER_INDEX)});
    Value *consumerGEP = b.CreateInBoundsGEP(
        devlinkType, link, {b.getInt64(0), b.getInt32(DEVLINK_CONSUMER_INDEX)});
    if (supplier->getType()->getPointerTo() != supplierGEP->getType())
      supplierGEP =
          b.CreateBitCast(supplierGEP, supplier->getType()->getPointerTo());
    if (consumer->getType()->getPointerTo() != consumerGEP->getType())
      consumerGEP =
          b.CreateBitCast(consumerGEP, consumer->getType()->getPointerTo());
    b.CreateStore(supplier, supplierGEP);
    b.CreateStore(consumer, consumerGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    PHINode *retPhi = b.CreatePHI(devlinkType->getPointerTo(), 2);
    retPhi->addIncoming(ConstantPointerNull::get(devlinkType->getPointerTo()),
                        entry);
    retPhi->addIncoming(link, body);
    b.CreateRet(retPhi);
  }

  void handleDeviceLinkAddAutoremoveCall(Module &m, Function *devlinkAdd) {
    auto isAutoremove = [](CallInst *devLinkAddCall) -> bool {
      ConstantInt *flags =
          dyn_cast<ConstantInt>(devLinkAddCall->getArgOperand(2));
      if (!flags)
        return false;
      return flags->getZExtValue() &
             (DL_FLAG_AUTOREMOVE_SUPPLIER | DL_FLAG_AUTOPROBE_CONSUMER);
    };

    Function *ndBool = getOrCreateNdIntFn(m, 1);
    for (CallInst *call : getCalls(devlinkAdd)) {
      if (!isAutoremove(call))
        continue;
      SmallVector<ICmpInst *, 8> icmps;
      for (User *user : call->users()) {
        if (ICmpInst *icmp = dyn_cast<ICmpInst>(user)) {
          icmps.push_back(icmp);
        } else {
          errs() << "The result of device_link_add with a autoremove flag "
                    "should only be used in a null check\n";
          errs() << "user in " << call->getFunction()->getName() << *user
                 << "\n";
          std::exit(1);
        }
      }
      Value *ndCond =
          CallInst::Create(ndBool, {}, "device_link_add.result", call);
      for (ICmpInst *icmp : icmps) {
        icmp->replaceAllUsesWith(ndCond);
        icmp->eraseFromParent();
      }
      call->eraseFromParent();
    }
  }

  void handleDeviceLinkDel(Module &m) {
    Function *f = m.getFunction("device_link_del");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.device_link_del");

    LLVMContext &ctx = m.getContext();
    Function *putDevice = m.getFunction("put_device");
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    Argument *link = f->getArg(0);
    Type *devlinkType = link->getType()->getPointerElementType();

    IRBuilder<> b(entry);
    Value *supplierGEP = b.CreateInBoundsGEP(
        devlinkType, link, {b.getInt64(0), b.getInt32(DEVLINK_SUPPLIER_INDEX)});
    Value *consumerGEP = b.CreateInBoundsGEP(
        devlinkType, link, {b.getInt64(0), b.getInt32(DEVLINK_CONSUMER_INDEX)});
    Type *devPtrType = putDevice->getArg(0)->getType();
    if (supplierGEP->getType() != devPtrType->getPointerTo())
      supplierGEP = b.CreateBitCast(supplierGEP, devPtrType->getPointerTo());
    if (consumerGEP->getType() != devPtrType->getPointerTo())
      consumerGEP = b.CreateBitCast(consumerGEP, devPtrType->getPointerTo());
    Value *supplier = b.CreateLoad(devPtrType, supplierGEP);
    Value *consumer = b.CreateLoad(devPtrType, consumerGEP);
    b.CreateCall(putDevice, supplier);
    b.CreateCall(putDevice, consumer);
    b.CreateRetVoid();
  }

  void handleDeviceLinkRemove(Module &m) {
    Function *f = m.getFunction("device_link_remove");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.device_link_remove");

    LLVMContext &ctx = m.getContext();
    Function *putDevice = m.getFunction("put_device");
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    Value *consumer = f->getArg(0);
    Value *supplier = f->getArg(1);
    Type *devPtrType = putDevice->getArg(0)->getType();

    IRBuilder<> b(entry);
    if (consumer->getType() != devPtrType)
      consumer = b.CreateBitCast(consumer, devPtrType);
    if (supplier->getType() != devPtrType)
      supplier = b.CreateBitCast(supplier, devPtrType);
    b.CreateCall(putDevice, consumer);
    b.CreateCall(putDevice, supplier);
    b.CreateRetVoid();
  }

  void handleDeviceFinders(Module &m, Function *updateIndex,
                           Attribute checkPointAttr) {
    const DenseMap<const GlobalVariable *, StructType *> &clsOrBusToDevType =
        clsOrBusToDeviceMap(m);
    SmallVector<GetElementPtrInst *> containerOfs;
    DenseMap<CallInst *, Value *> findCallToSurroundingDevPtr;
    for (StringRef name : {"class_find_device", "bus_find_device",
                           "device_create_with_groups"}) {
      Function *finder = m.getFunction(name);
      if (!finder)
        continue;
      for (CallInst *call : getCalls(finder)) {
        StructType *surroundingDevType =
            getSurroundingDevType(m, call, clsOrBusToDevType);
        StructType *devType =
            cast<StructType>(call->getType()->getPointerElementType());
        if (!surroundingDevType) {
          surroundingDevType = devType;
        }
        const SmallVector<Value *> &devIndices =
            gepIndicesToStruct(surroundingDevType, devType).getValue();
        Function *devGetter = deviceGetter(m, surroundingDevType, devIndices,
                                           updateIndex, checkPointAttr);
        IRBuilder<> b(call);
        Value *replace = b.CreateCall(devGetter);
        if (replace->getType() != call->getType())
          replace = b.CreateBitCast(replace, call->getType());
        call->replaceAllUsesWith(replace);
        call->eraseFromParent();
        findCallToSurroundingDevPtr[call] = replace;
        collectContainersOfs(call, containerOfs);
      }
    }
    replaceContainerOfs(containerOfs, findCallToSurroundingDevPtr);
  }

  void collectContainersOfs(CallInst *call,
                            SmallVector<GetElementPtrInst *> &containerOfs) {
    SmallVector<User *> users(call->user_begin(), call->user_end());
    DenseSet<User *> visited;
    while (!users.empty()) {
      User *user = users.pop_back_val();
      if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(user)) {
        if (const ConstantInt *c = dyn_cast<ConstantInt>(gep->getOperand(1))) {
          if (c->isNegative())
            containerOfs.push_back(gep);
        }
      } else if (PHINode *phi = dyn_cast<PHINode>(user)) {
        for (User *u : phi->users()) {
          if (visited.insert(u).second)
            users.push_back(u);
        }
      }
    }
  }

  bool isClsOrBusPtr(Type *type) {
    if (PointerType *ptr = dyn_cast<PointerType>(type)) {
      if (StructType *s = dyn_cast<StructType>(ptr->getElementType()))
        return s->getName().startswith("struct.class.") ||
               s->getName().startswith("struct.bus_type.") ||
               s->getName().equals("struct.class") ||
               s->getName().equals("struct.bus_type");
    }
    return false;
  }

  SmallVector<StructType *> getBaseType(const Value *v) {
    SmallVector<StructType *> res;
    const StructType *deviceType =
        StructType::getTypeByName(v->getContext(), "struct.device");
    auto recordTypeIfEmbedsDevice = [&res, deviceType](Type *type) {
      if (StructType *s = dyn_cast<StructType>(type)) {
        if (embedsStruct(s, deviceType))
          res.push_back(s);
      }
    };

    const Value *base = getUnderlyingObject(v);
    if (const CallInst *call = dyn_cast<CallInst>(base)) {
      const Function *f = extractCalledFunction(call);
      if (f->getName().equals("drvhorn.alloc")) {
        // guess the actual type for the drvhorn.alloc call.
        SmallVector<const User *> workList(call->user_begin(),
                                           call->user_end());

        DenseSet<const User *> visited;
        while (!workList.empty()) {
          const User *user = workList.pop_back_val();
          if (const BitCastOperator *bitcast =
                  dyn_cast<BitCastOperator>(user)) {
            for (const User *u : user->users()) {
              if (visited.insert(u).second)
                workList.push_back(u);
            }
            recordTypeIfEmbedsDevice(
                bitcast->getDestTy()->getPointerElementType());
          } else if (const ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
            Type *retValType =
                ret->getReturnValue()->getType()->getPointerElementType();
            recordTypeIfEmbedsDevice(retValType);
          } else if (const PHINode *phi = dyn_cast<PHINode>(user)) {
            for (const User *u : phi->users()) {
              if (visited.insert(u).second)
                workList.push_back(u);
            }
          } else if (const StoreInst *store = dyn_cast<StoreInst>(user)) {
            Type *valType = store->getValueOperand()->getType();
            if (valType->isPointerTy() &&
                valType->getPointerElementType()->isIntegerTy(8)) {
              const Value *ptr = store->getPointerOperand();
              // %a = bitcast %struct.some_dev** to i8*
              // store i8*, i8** %a
              Type *strippedType = ptr->stripPointerCasts()->getType();
              if (strippedType->isPointerTy() &&
                  strippedType->getPointerElementType()->isPointerTy()) {
                recordTypeIfEmbedsDevice(strippedType->getPointerElementType()
                                             ->getPointerElementType());
              }
            }
          }
        }
      }
    }
    recordTypeIfEmbedsDevice(base->getType()->getPointerElementType());
    return res;
  }

  StructType *getDeviceTypeForClsOrBus(const GlobalVariable *gv, bool isPtr) {
    SmallVector<const User *> clsOrBus;
    if (!isPtr) {
      clsOrBus.push_back(gv);
    } else {
      SmallVector<const User *> workList(gv->users());
      DenseSet<const User *> visitedUsers(gv->user_begin(), gv->user_end());
      while (!workList.empty()) {
        const User *user = workList.pop_back_val();
        if (isa<BitCastOperator>(user)) {
          for (const User *u : user->users()) {
            if (visitedUsers.insert(u).second)
              workList.push_back(u);
          }
        } else if (isa<LoadInst>(user)) {
          clsOrBus.push_back(user);
        }
      }
    }
    DenseSet<const User *> visited;
    SmallVector<const User *> users;
    for (const User *p : clsOrBus) {
      for (const User *u : p->users()) {
        if (visited.insert(u).second)
          users.push_back(u);
      }
    }
    SmallVector<StructType *> baseTypes;
    while (!users.empty()) {
      const User *user = users.pop_back_val();
      if (isa<BitCastOperator>(user)) {
        for (const User *u : user->users()) {
          if (visited.insert(u).second)
            users.push_back(u);
        }
      } else if (const StoreInst *store = dyn_cast<StoreInst>(user)) {
        if (isClsOrBusPtr(store->getValueOperand()->getType())) {
          SmallVector<StructType *> baseType =
              getBaseType(store->getPointerOperand());
          baseTypes.append(baseType.begin(), baseType.end());
        }
      }
    }
    if (baseTypes.empty()) {
      return nullptr;
    }
    StructType *cur = baseTypes[0];
    for (size_t i = 1; i < baseTypes.size(); i++) {
      if (embedsStruct(baseTypes[i], cur)) {
        cur = baseTypes[i];
      }
    }
    return cur;
  }

  DenseMap<const GlobalVariable *, StructType *>
  clsOrBusToDeviceMap(const Module &m) {
    DenseMap<const GlobalVariable *, StructType *> map;
    LLVMContext &ctx = m.getContext();
    StructType *clsType = StructType::getTypeByName(ctx, "struct.class");
    StructType *busType = StructType::getTypeByName(ctx, "struct.bus_type");
    for (const GlobalVariable &gv : m.globals()) {
      Type *type = gv.getValueType();
      bool isPtr = false;
      if (type->isPointerTy()) {
        isPtr = true;
        type = type->getPointerElementType();
      }
      if (equivTypes(type, clsType) || equivTypes(type, busType)) {
        StructType *devType = getDeviceTypeForClsOrBus(&gv, isPtr);
        map[&gv] = devType;
      }
    }
    return map;
  }

  StructType *getSurroundingDevType(
      Module &m, CallInst *call,
      const DenseMap<const GlobalVariable *, StructType *> &clsOrBusToDevType) {
    Value *argBase = call->getArgOperand(0)->stripPointerCasts();
    GlobalVariable *gv = nullptr;
    if (GlobalVariable *g = dyn_cast<GlobalVariable>(argBase)) {
      gv = g;
    } else if (LoadInst *load = dyn_cast<LoadInst>(argBase)) {
      gv = dyn_cast<GlobalVariable>(
          load->getPointerOperand()->stripPointerCasts());
    } else if (Argument *arg = dyn_cast<Argument>(argBase)) {
      return nullptr;
    } else if (CallInst *c = dyn_cast<CallInst>(argBase)) {
      Function *f = extractCalledFunction(c);
      if (!f->getName().equals("class_create")) {
        errs() << "TODO: getSurroundingDevTyps call " << *c << "\n";
        std::exit(1);
      }
      for (User *u : c->users()) {
        if (StoreInst *store = dyn_cast<StoreInst>(u)) {
          if (GlobalVariable *g = dyn_cast<GlobalVariable>(
                  store->getPointerOperand()->stripPointerCasts())) {
            gv = g;
            break;
          }
        }
      }
    } else {
      errs() << "TODO: getSurroundingDevTyps " << *argBase << " in "
             << call->getFunction()->getName() << "\n";
      std::exit(1);
    }
    return clsOrBusToDevType.lookup(gv);
  }

  // returns a pointer to the surrounding device.
  Function *deviceGetter(Module &m, StructType *surroundingDevType,
                         const SmallVector<Value *> &devIndices,
                         Function *updateIndex, Attribute checkPointAttr) {
    LLVMContext &ctx = m.getContext();
    Type *retType = getGEPType(surroundingDevType, devIndices);
    std::string getterName =
        "drvhorn.device_getter." + surroundingDevType->getName().str();
    if (Function *getter = m.getFunction(getterName))
      return getter;
    Function *getter =
        Function::Create(FunctionType::get(retType->getPointerTo(), false),
                         GlobalValue::PrivateLinkage, getterName, &m);
    Function *gen = buildStorageElemGenerator(
        m, surroundingDevType, updateIndex, devIndices, checkPointAttr);
    Constant *getDevice = m.getFunction("get_device");
    FunctionType *getDeviceType =
        FunctionType::get(gen->getReturnType(), gen->getReturnType(), false);
    if (getDevice->getType() != getDeviceType->getPointerTo()) {
      getDevice =
          ConstantExpr::getBitCast(getDevice, getDeviceType->getPointerTo());
    }

    BasicBlock *entry = BasicBlock::Create(ctx, "entry", getter);
    BasicBlock *body = BasicBlock::Create(ctx, "body", getter);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", getter);

    IRBuilder<> b(entry);
    Value *dev = b.CreateCall(gen);
    Value *isNull = b.CreateIsNull(dev);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    b.CreateCall(getDeviceType, getDevice, dev);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRet(dev);
    return getter;
  }

  void replaceContainerOfs(
      ArrayRef<GetElementPtrInst *> containerOfs,
      const DenseMap<CallInst *, Value *> &findCallToSurroundingDevPtr) {

    auto replace = [findCallToSurroundingDevPtr](CallInst *call,
                                                 GetElementPtrInst *gep) {
      Value *surroundingDevPtr = findCallToSurroundingDevPtr.lookup(call);
      if (!surroundingDevPtr) {
        errs() << "replaceContainerOfs: no surroundingDevPtr for " << *call
               << "\n";
        std::exit(1);
      }
      ContainerOfVisitor replacer(gep, surroundingDevPtr);
      SmallVector<User *> users(gep->user_begin(), gep->user_end());
      for (User *user : users) {
        if (Instruction *inst = dyn_cast<Instruction>(user)) {
          replacer.visit(inst);
        }
      }
    };

    for (GetElementPtrInst *gep : containerOfs) {
      Value *v = getUnderlyingObject(gep);
      if (CallInst *call = dyn_cast<CallInst>(v)) {
        replace(call, gep);
      } else if (PHINode *phi = dyn_cast<PHINode>(v)) {
        for (Value *v : phi->incoming_values()) {
          if (CallInst *call = dyn_cast<CallInst>(v)) {
            replace(call, gep);
          } else if (!isa<Constant>(v)) {
            errs() << "replaceContainerOfs: unexpected PHINode " << *phi
                   << "\n";
            std::exit(1);
          }
        }
      } else {
        errs() << "replaceContainerOfs: unexpected container_of " << *v << "\n";
        std::exit(1);
      }
      gep->eraseFromParent();
    }
  }

  Function *handleDeviceInitialize(Module &m, Attribute checkPointAttr) {
    Function *f = m.getFunction("device_initialize");
    if (!f)
      return nullptr;
    f->deleteBody();
    f->setName("drvhorn.device_initialize");
    LLVMContext &ctx = m.getContext();
    Function *krefGet = m.getFunction("drvhorn.kref_get");
    StructType *krefType = cast<StructType>(
        krefGet->getArg(0)->getType()->getPointerElementType());
    Argument *dev = f->getArg(0);
    StructType *devType =
        cast<StructType>(dev->getType()->getPointerElementType());
    BasicBlock *blk = BasicBlock::Create(ctx, "blk", f);
    IRBuilder<> b(blk);
    Value *krefGEP = b.CreateInBoundsGEP(
        devType, dev, gepIndicesToStruct(devType, krefType).getValue());
    b.CreateCall(krefGet, krefGEP);
    b.CreateRetVoid();

    for (CallInst *call : getCalls(f)) {
      handleDeviceReleaseFunctions(call, checkPointAttr);
    }
    return f;
  }

  void handleDeviceReleaseFunctions(CallInst *call, Attribute checkPointAttr) {
    handleTypeDeviceReleaseFunctions(call, checkPointAttr);
    handleClassDeviceReleaseFunctions(call, checkPointAttr);
  }

#define TYPE_DEV_RELEASE_INDEX 4
  void handleTypeDeviceReleaseFunctions(CallInst *call,
                                        Attribute checkPointAttr) {
    LLVMContext &ctx = call->getContext();
    StructType *deviceType =
        StructType::getTypeByName(ctx, "struct.device_type");
    for (Instruction &inst : instructions(call->getFunction())) {
      if (StoreInst *store = dyn_cast<StoreInst>(&inst)) {
        GlobalVariable *gv = dyn_cast<GlobalVariable>(
            store->getValueOperand()->stripPointerCasts());
        GEPOperator *typeGEP = dyn_cast<GEPOperator>(
            store->getPointerOperand()->stripPointerCasts());
        if (!gv || !equivTypes(gv->getValueType(), deviceType) || !typeGEP)
          continue;
        Function *release =
            dyn_cast<Function>(gv->getInitializer()
                                   ->getAggregateElement(TYPE_DEV_RELEASE_INDEX)
                                   ->stripPointerCasts());
        if (!release)
          continue;
        release->setName("drvhorn." + release->getName());
        release->addFnAttr(checkPointAttr);

        // TODO: call when the refcount is 0.
        IRBuilder<> b(call->getParent()->getTerminator());
        Value *dev = call->getArgOperand(0);
        if (dev->getType() != release->getArg(0)->getType())
          dev = b.CreateBitCast(dev, release->getArg(0)->getType());
        b.CreateCall(release, dev);
      }
    }
  }

#define CLASS_DEV_RELEASE_INDEX 6
  void handleClassDeviceReleaseFunctions(CallInst *call,
                                         Attribute checkPointAttr) {
    LLVMContext &ctx = call->getContext();
    StructType *classType = StructType::getTypeByName(ctx, "struct.class");
    for (Instruction &inst : instructions(call->getFunction())) {
      if (StoreInst *store = dyn_cast<StoreInst>(&inst)) {
        GlobalVariable *gv = dyn_cast<GlobalVariable>(
            store->getValueOperand()->stripPointerCasts());
        GEPOperator *clsGEP = dyn_cast<GEPOperator>(
            store->getPointerOperand()->stripPointerCasts());
        if (!gv || !equivTypes(gv->getValueType(), classType) || !clsGEP)
          continue;
        Function *devRelease = dyn_cast<Function>(
            gv->getInitializer()
                ->getAggregateElement(CLASS_DEV_RELEASE_INDEX)
                ->stripPointerCasts());
        if (!devRelease)
          continue;
        devRelease->setName("drvhorn." + devRelease->getName());
        devRelease->addFnAttr(checkPointAttr);

        // TODO: call when the refcount is 0.
        IRBuilder<> b(call->getParent()->getTerminator());
        Value *dev = call->getArgOperand(0);
        if (dev->getType() != devRelease->getArg(0)->getType())
          dev = b.CreateBitCast(dev, devRelease->getArg(0)->getType());
        b.CreateCall(devRelease, dev);
      }
    }
  }

  void handleDeviceAllocation(Module &m, Function *devInit,
                              Function *updateIndex, Attribute checkPointAttr) {
    if (!devInit)
      return;
    Function *alloc = getOrCreateAlloc(m);
    DeviceGEPGetter getter;
    const DenseMap<uint64_t, SmallVector<StructType *>> &structsBySize =
        getStructsBySize(m);
    for (CallInst *call : getCalls(alloc)) {
      Optional<DeviceGEP> devGEP = getter.getGEPIndices(call);
      StructType *allocatedDevType =
          getCustomDevType(m, structsBySize, devGEP, call);
      if (!allocatedDevType)
        continue;
      Function *devAlloc = buildStorageElemGenerator(
          m, allocatedDevType, updateIndex, {}, checkPointAttr);
      IRBuilder<> b(call);
      CallInst *newCall = b.CreateCall(devAlloc);
      Value *replace = b.CreateBitCast(newCall, call->getType());
      call->replaceAllUsesWith(replace);
      call->eraseFromParent();
    }
  }

  DenseMap<uint64_t, SmallVector<StructType *>> getStructsBySize(Module &m) {
    DenseMap<uint64_t, SmallVector<StructType *>> res;
    DataLayout dl(&m);
    for (StructType *st : m.getIdentifiedStructTypes()) {
      uint64_t size = dl.getTypeAllocSize(st);
      res[size].push_back(st);
    }
    return res;
  }

  StructType *getCustomDevType(
      Module &m,
      const DenseMap<uint64_t, SmallVector<StructType *>> &structsBySize,
      const Optional<DeviceGEP> &devGEP, CallInst *call) {
    DataLayout dl(&m);
    LLVMContext &ctx = m.getContext();
    StructType *devType = StructType::getTypeByName(ctx, "struct.device");
    ConstantInt *sizeArg = dyn_cast<ConstantInt>(call->getArgOperand(0));
    if (!sizeArg)
      return nullptr;
    uint64_t size = sizeArg->getZExtValue();
    if (StructType *directType = directlyCastedType(call)) {
      if (dl.getTypeAllocSize(directType) == size &&
          embedsStruct(directType, devType))
        return directType;
    }
    if (!devGEP.hasValue())
      return nullptr;
    for (StructType *st : structsBySize.lookup(size)) {
      if (hasDeviceAtIndices(st, *devGEP, dl))
        return st;
    }
    return createCustomDevType(size, *devGEP, dl);
  }

  StructType *directlyCastedType(CallInst *alloc) {
    for (User *user : alloc->users()) {
      if (BitCastOperator *bitcast = dyn_cast<BitCastOperator>(user)) {
        if (StructType *st = dyn_cast<StructType>(
                bitcast->getDestTy()->getPointerElementType()))
          return st;
      }
    }
    return nullptr;
  }

  bool hasDeviceAtIndices(StructType *st, DeviceGEP deviceGEP, DataLayout &dl) {
    if (!st)
      return false;
    for (Type *elemType : st->elements()) {
      uint64_t elemSize = dl.getTypeAllocSize(elemType);
      if (deviceGEP.index == 0)
        return equivTypes(elemType, deviceGEP.devType);
      if (elemSize > deviceGEP.index)
        return hasDeviceAtIndices(dyn_cast<StructType>(elemType), deviceGEP,
                                  dl);
      if (elemSize == deviceGEP.index)
        return equivTypes(elemType, deviceGEP.devType);
      deviceGEP.index -= elemSize;
    }
    return false;
  }

  StructType *createCustomDevType(uint64_t size, DeviceGEP devGEP,
                                  DataLayout &dl) {
    IntegerType *i8Ty = Type::getInt8Ty(devGEP.devType->getContext());
    ArrayType *prePaddingType = nullptr;
    if (devGEP.index)
      prePaddingType = ArrayType::get(i8Ty, devGEP.index);
    ArrayType *postPaddingType = nullptr;
    uint64_t postSize =
        size - devGEP.index - dl.getTypeAllocSize(devGEP.devType);
    if (postSize)
      postPaddingType = ArrayType::get(i8Ty, postSize);
    SmallVector<Type *> elements;
    if (prePaddingType)
      elements.push_back(prePaddingType);
    elements.push_back(devGEP.devType);
    if (postPaddingType)
      elements.push_back(postPaddingType);
    return StructType::create(elements, "custom");
  }

#define OF_PHANDLE_ARG_DEVNODE_INDEX 0
  void handleOfParsePhandleWithArgs(Module &m, Function *devNodeGetter) {
    LLVMContext &ctx = m.getContext();
    StringRef names[] = {"__of_parse_phandle_with_args",
                         "of_parse_phandle_with_args_map"};
    Constant *ofNodeGet = m.getFunction("of_node_get");
    for (StringRef name : names) {
      Function *f = m.getFunction(name);
      if (!f)
        return;
      f->deleteBody();
      f->setName("drvhorn." + name);
      Argument *outArg = f->getArg(f->arg_size() - 1);
      BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
      BasicBlock *body = BasicBlock::Create(ctx, "body", f);
      BasicBlock *store = BasicBlock::Create(ctx, "store", f);
      BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

      IRBuilder<> b(entry);
      Value *isArgNull = b.CreateIsNull(outArg);
      b.CreateCondBr(isArgNull, ret, body);

      b.SetInsertPoint(body);
      Value *devNode = b.CreateCall(devNodeGetter);
      FunctionType *ofNodeGetType =
          FunctionType::get(devNode->getType(), devNode->getType(), false);
      if (ofNodeGet->getType() != ofNodeGetType->getPointerTo())
        ofNodeGet =
            ConstantExpr::getBitCast(ofNodeGet, ofNodeGetType->getPointerTo());
      b.CreateCall(ofNodeGetType, ofNodeGet, devNode);
      Value *isNodeNull = b.CreateIsNull(devNode);
      b.CreateCondBr(isNodeNull, ret, store);

      b.SetInsertPoint(store);
      Value *devNodeGEP = b.CreateInBoundsGEP(
          outArg->getType()->getPointerElementType(), outArg,
          {b.getInt64(0), b.getInt32(OF_PHANDLE_ARG_DEVNODE_INDEX)});
      b.CreateStore(devNode, devNodeGEP);
      b.CreateBr(ret);

      b.SetInsertPoint(ret);
      PHINode *phi = b.CreatePHI(f->getReturnType(), 3);
      phi->addIncoming(b.getInt32(0), entry);
      phi->addIncoming(b.getInt32(-ENOENT), body);
      phi->addIncoming(b.getInt32(0), store);
      b.CreateRet(phi);
    }
  }

#define OF_PHANDLE_ITERATOR_DEVNODE_INDEX 8
  void handleOfPhandleIteratorNext(Module &m, Function *devNodeGetter) {
    Function *f = m.getFunction("of_phandle_iterator_next");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.of_phandle_iterator_next");
    LLVMContext &ctx = m.getContext();
    Argument *itArg = f->getArg(0);
    Constant *ofNodeGet = m.getFunction("of_node_get");
    Constant *ofNodePut = m.getFunction("of_node_put");
    Type *voidTy = Type::getVoidTy(ctx);
    BasicBlock *blk = BasicBlock::Create(ctx, "blk", f);
    IRBuilder<> b(blk);
    Value *devNodeGEP = b.CreateInBoundsGEP(
        itArg->getType()->getPointerElementType(), itArg,
        {b.getInt64(0), b.getInt32(OF_PHANDLE_ITERATOR_DEVNODE_INDEX)});
    Type *devNodeType = devNodeGEP->getType()->getPointerElementType();
    FunctionType *ofNodePutType = FunctionType::get(voidTy, devNodeType, false);
    if (ofNodePut->getType() != ofNodePutType->getPointerTo())
      ofNodePut =
          ConstantExpr::getBitCast(ofNodePut, ofNodePutType->getPointerTo());
    LoadInst *node = b.CreateLoad(devNodeType, devNodeGEP);
    b.CreateCall(ofNodePutType, ofNodePut, node);
    Value *newDevNode = b.CreateCall(devNodeGetter);
    FunctionType *ofNodeGetType =
        FunctionType::get(devNodeType, devNodeType, false);
    if (ofNodeGet->getType() != ofNodeGetType->getPointerTo())
      ofNodeGet =
          ConstantExpr::getBitCast(ofNodeGet, ofNodeGetType->getPointerTo());
    b.CreateCall(ofNodeGetType, ofNodeGet, newDevNode);
    if (newDevNode->getType() != devNodeType)
      newDevNode = b.CreateBitCast(newDevNode, devNodeType);
    b.CreateStore(newDevNode, devNodeGEP);
    Value *ok = b.CreateIsNotNull(newDevNode);
    Value *ret = b.CreateSelect(ok, b.getInt32(0), b.getInt32(-EINVAL));
    b.CreateRet(ret);
  }

  void ignoreSomeFunctions(Module &m) {
    StringRef names[] = {
        "device_add",
        "device_del",
        "device_destroy", // TODO: device_destroy decrements the refcount, but
                          // we don't have a way to get the target device.
        "kobject_uevent_env",
        "__of_translate_address",
        "of_count_phandle_with_args",
        "of_irq_get",
        "of_irq_parse_raw",
        "of_irq_parse_one",
        "of_platform_populate",
        "of_cpu_node_to_id",
        "class_for_each_device",
        "mdiobus_scan",
        // clk
        "clk_get",
        "clk_put",
        "of_clk_add_hw_provider",
        // devm functions that confirmed to be refcount bug-free
        "devm_of_clk_add_hw_provider",
        "__devm_of_phy_provider_register",
        "devm_regulator_get",
        "devm_regulator_get_exclusive",
        "devm_regulator_get_enable_optional",
        "devm_regulator_get_enable",
        "devm_regulator_get_optional",
        "devm_regulator_bulk_get",
        "devm_regulator_bulk_get_exclusive",
        "devm_regulator_bulk_get_const",
        "devm_regulator_register",
        "devm_led_classdev_register_ext",
        "devm_phy_create",
        "devm_gpiochip_add_data_with_key",
        // this function frees memory manually
        "gpiochip_add_data_with_key",
    };
    for (StringRef name : names) {
      if (Function *f = m.getFunction(name))
        f->deleteBody();
    }
  }

  void stubFwnodeConnectionFindMatch(Module &m) {
    Function *f = m.getFunction("fwnode_connection_find_match");
    if (!f)
      return;
    IntegerType *i8Ty = Type::getInt8Ty(m.getContext());
    for (CallInst *call : getCalls(f)) {
      IRBuilder<> b(call);
      AllocaInst *ret = b.CreateAlloca(i8Ty, b.getInt32(0x10000));
      call->replaceAllUsesWith(ret);
      call->eraseFromParent();
    }
  }

  void stubFwnodeConnectionFindMatches(Module &m) {
    Function *f = m.getFunction("fwnode_connection_find_matches");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.fwnode_connection_find_matches");

    LLVMContext &ctx = m.getContext();
    SeaBuiltinsInfo &sbi = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    Function *assumeFn = sbi.mkSeaBuiltinFn(SeaBuiltinsOp::ASSUME, m);
    Function *ndBool = getOrCreateNdIntFn(m, 1);
    Function *ndI32 = getOrCreateNdIntFn(m, 32);
    IntegerType *i32Ty = Type::getInt32Ty(ctx);

    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Value *cond = b.CreateCall(ndBool);
    b.CreateCondBr(cond, body, ret);

    b.SetInsertPoint(body);
    Value *retVal = b.CreateCall(ndI32);
    Argument *len = f->getArg(5);
    Value *withinRange = b.CreateAnd(b.CreateICmpULT(retVal, len),
                                     b.CreateICmpSGE(retVal, b.getInt32(0)));
    b.CreateCall(assumeFn, withinRange);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    PHINode *retPhi = b.CreatePHI(i32Ty, 2);
    retPhi->addIncoming(b.getInt32(-EINVAL), entry);
    retPhi->addIncoming(retVal, body);
    b.CreateRet(retPhi);
  }

  // TODO: implement in a different file or rename this file.
  void handleCpufreqGet(Module &m, Function *updateIndex,
                        Attribute checkPointAttr) {
    Function *f = m.getFunction("cpufreq_cpu_get");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.cpufreq_cpu_get");
    LLVMContext &ctx = m.getContext();
    f->addFnAttr(checkPointAttr);
    StructType *policyType =
        cast<StructType>(f->getReturnType()->getPointerElementType());

    Function *gen = buildStorageElemGenerator(m, policyType, updateIndex, {});

    Function *krefGet = m.getFunction("drvhorn.kref_get");
    Type *krefType = krefGet->getArg(0)->getType()->getPointerElementType();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Value *policy = b.CreateCall(gen);
    Value *isNull = b.CreateIsNull(policy);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    Value *krefPtr = b.CreateInBoundsGEP(
        policyType, policy,
        gepIndicesToStruct(policyType, krefType).getValue());
    b.CreateCall(krefGet, krefPtr);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    b.CreateRet(policy);
  }

  void handleRegulatorGet(Module &m, Function *updateIndex,
                          Attribute checkPointAttr) {
    auto getRegulatorDevTypeAndIndex =
        [&m](StructType *regulatorType) -> std::pair<StructType *, unsigned> {
      StructType *regulatorDevType =
          StructType::getTypeByName(m.getContext(), "struct.regulator_dev");
      for (unsigned i = 0; i < regulatorType->getNumElements(); i++) {
        Type *elem = regulatorType->getElementType(i);
        if (equivTypes(elem, regulatorDevType->getPointerTo()))
          return {cast<StructType>(elem->getPointerElementType()), i};
      }
      return {nullptr, 0};
    };

    Function *f = m.getFunction("_regulator_get");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn._regulator_get");
    LLVMContext &ctx = m.getContext();
    f->addFnAttr(checkPointAttr);
    StructType *regulatorType =
        cast<StructType>(f->getReturnType()->getPointerElementType());
    std::pair<StructType *, unsigned> regDevTypeAndIndex =
        getRegulatorDevTypeAndIndex(regulatorType);
    StructType *regulatorDevType = regDevTypeAndIndex.first;
    unsigned regulatorDevIndex = regDevTypeAndIndex.second;

    Function *gen =
        buildStorageElemGenerator(m, regulatorDevType, updateIndex, {});

    Function *krefGet = m.getFunction("drvhorn.kref_get");
    Type *krefType = krefGet->getArg(0)->getType()->getPointerElementType();
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);
    BasicBlock *body = BasicBlock::Create(ctx, "body", f);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", f);

    IRBuilder<> b(entry);
    Value *regulatorDev = b.CreateCall(gen);
    Value *isNull = b.CreateIsNull(regulatorDev);
    b.CreateCondBr(isNull, ret, body);

    b.SetInsertPoint(body);
    Value *krefPtr = b.CreateInBoundsGEP(
        regulatorDevType, regulatorDev,
        gepIndicesToStruct(regulatorDevType, krefType).getValue());
    b.CreateCall(krefGet, krefPtr);
    AllocaInst *regulator = b.CreateAlloca(regulatorType);
    Value *regulatorDevGEP =
        b.CreateInBoundsGEP(regulatorType, regulator,
                            {b.getInt64(0), b.getInt32(regulatorDevIndex)});
    b.CreateStore(regulatorDev, regulatorDevGEP);
    b.CreateBr(ret);

    b.SetInsertPoint(ret);
    PHINode *retPhi = b.CreatePHI(regulatorType->getPointerTo(), 2);
    retPhi->addIncoming(ConstantPointerNull::get(regulatorType->getPointerTo()),
                        entry);
    retPhi->addIncoming(regulator, body);
    b.CreateRet(retPhi);
  }

  // handle led_classdev_register_ext since it calls led_classdev_next_name
  // which will be reduced to an infity loop and be a huge source of false
  // alarms.
  void handleLedRegister(Module &m, Function *updateIndex,
                         Attribute checkPointAttr) {
    Function *f = m.getFunction("led_classdev_register_ext");
    if (!f)
      return;
    f->deleteBody();
    f->setName("drvhorn.led_classdev_register_ext");
    f->addFnAttr(checkPointAttr);

    Argument *ledClassDevArg = f->getArg(1);
    StructType *ledClassDevType =
        cast<StructType>(ledClassDevArg->getType()->getPointerElementType());
    LLVMContext &ctx = m.getContext();
    const Optional<SmallVector<Value *>> &gep(gepIndicesToStruct(
        ledClassDevType,
        StructType::getTypeByName(ctx, "struct.device")->getPointerTo()));
    StructType *devType = cast<StructType>(
        getGEPType(ledClassDevType, *gep)->getPointerElementType());
    Function *devGetter =
        deviceGetter(m, devType, {}, updateIndex, checkPointAttr);

    BasicBlock *entry = BasicBlock::Create(ctx, "entry", f);

    IRBuilder<> b(entry);
    Value *dev = b.CreateCall(devGetter);
    Value *devGEP = b.CreateInBoundsGEP(ledClassDevType, ledClassDevArg, *gep);
    b.CreateStore(dev, devGEP);
    Value *isNull = b.CreateIsNull(dev);
    Value *ret = b.CreateSelect(isNull, b.getInt32(-1), b.getInt32(0));
    b.CreateRet(ret);
  }
};

char HandleDevices::ID = 0;

Pass *createHandleDevicesPass() { return new HandleDevices(); }
}; // namespace seahorn
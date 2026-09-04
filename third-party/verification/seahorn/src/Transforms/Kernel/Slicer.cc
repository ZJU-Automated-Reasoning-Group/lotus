#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"

#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

#define COMPILER_USED_NAME "llvm.compiler.used"

using namespace llvm;

namespace seahorn {

static bool embedsStructIndirect(Type *type, Type *inner) {
  if (ArrayType *arr = dyn_cast<ArrayType>(type)) {
    return embedsStructIndirect(arr->getElementType(), inner);
  } else if (PointerType *ptr = dyn_cast<PointerType>(type)) {
    return embedsStructIndirect(ptr->getElementType(), inner);
  } else if (StructType *st = dyn_cast<StructType>(type)) {
    return embedsStruct(st, inner);
  }
  return false;
}

struct Acceptor : public InstVisitor<Acceptor> {
  Acceptor(DenseSet<const Instruction *> &targets,
           DenseSet<const Value *> &underlyingLoadedPtrs)
      : targets(targets), underlyingLoadedPtrs(underlyingLoadedPtrs) {}

  void visitValue(Value *v) {
    if (!visited.insert(v).second)
      return;
    if (Instruction *inst = dyn_cast<Instruction>(v)) {
      visit(inst);
    } else if (Operator *op = dyn_cast<Operator>(v)) {
      visitOperator(op);
    }
  }

  void visitInstruction(Instruction &inst) {
    targets.insert(&inst);
    for (Value *v : inst.operands()) {
      visitValue(v);
    }
  }

  void visitCallInst(CallInst &call) {}

  void visitLoadInst(LoadInst &load) {
    targets.insert(&load);
    Value *ptr = load.getPointerOperand();
    Value *v = getUnderlyingObject(ptr);
    if (GlobalVariable *gv = dyn_cast<GlobalVariable>(v)) {
      if (!gv->getName().startswith("drvhorn."))
        return;
    }
    underlyingLoadedPtrs.insert(v);
    visitValue(ptr);
  }

private:
  DenseSet<const Instruction *> &targets;
  DenseSet<const Value *> &underlyingLoadedPtrs;
  DenseSet<const Value *> visited;

  void visitOperator(Operator *op) {
    if (isa<BitCastOperator, GEPOperator>(op))
      visitValue(op->getOperand(0));
  }
};

struct Filter : public InstVisitor<Filter, bool> {
public:
  Filter(Module &m) {
    const DenseSet<const CallInst *> &ignoreList = trivialFwnodeGetters(m);
    buildGenerators(m, ignoreList);
    buildTargetArgs(m);

    for (Function &f : m) {
      // First, track return insts.
      for (BasicBlock &blk : f) {
        if (ReturnInst *ret = dyn_cast<ReturnInst>(blk.getTerminator())) {
          visitReturnInst(*ret);
          recordUnderlyingObjectOfRetVal(ret);
        }
      }

      // store and some calls cannot be tracked from ret.
      for (Instruction &inst : instructions(f)) {
        if (isa<CallInst>(inst))
          visit(inst);
      }

      // track stores last, since some underlyingLoadedPtrs might be inserted
      // during the previous loop.
      while (true) {
        size_t size = underlyingTargetPtrs.size();
        for (Instruction &inst : instructions(f)) {
          if (isa<StoreInst>(inst))
            visit(inst);
        }
        if (size == underlyingTargetPtrs.size())
          break;
      }
    }
  }

  bool isTarget(const Instruction *inst) {
    if (inst->isTerminator())
      return true;
    return targets.count(inst);
  }

  bool visitReturnInst(ReturnInst &ret) {
    recordInst(&ret);
    if (Value *retVal = ret.getReturnValue()) {
      visitValue(retVal);
    }
    return true;
  }

  bool visitLoadInst(LoadInst &load) {
    Value *ptr = load.getPointerOperand();
    bool isTarget = visitValue(ptr);
    if (isTarget) {
      recordInst(&load);
      underlyingTargetPtrs.insert(getUnderlyingObject(ptr));
    }
    return isTarget;
  }

  bool visitStoreInst(StoreInst &store) {
    bool isTarget = isStoreTarget(store);
    if (isTarget) {
      recordInst(&store);
      Acceptor acceptor(targets, underlyingTargetPtrs);
      acceptor.visit(store);
    }
    return isTarget;
  }

  bool visitCallInst(CallInst &call) {
    bool isTarget = refcountGenerators.count(&call);
    if (Function *f = extractCalledFunction(call)) {
      bool isDevm = f->hasFnAttribute("drvhorn.devm");
      if (isDevm) {
        underlyingTargetPtrs.insert(&call);
      }
      for (Argument &arg : f->args()) {
        if (targetArgs.count(&arg) || isDevm) {
          isTarget = true;
          Acceptor acceptor(targets, underlyingTargetPtrs);
          Value *argVal = call.getArgOperand(arg.getArgNo());
          acceptor.visitValue(argVal);
        }
      }

      if (f->hasFnAttribute("drvhorn.fwnode")) {
        // ACPI handles refercount differently, so simply ignore them.
        if (call.getFunction()->getName().contains("acpi_")) {
          isTarget = false;
        }
      }
    }
    if (isTarget) {
      recordInst(&call);
      for (Value *v : call.operands()) {
        visitValue(v);
      }
    }
    return isTarget;
  }

  bool visitBranchInst(BranchInst &br) {
    if (br.isConditional()) {
      visitValue(br.getCondition());
    }
    recordInst(&br);
    return true;
  }

  bool visitPHINode(PHINode &phi) {
    for (Value *v : phi.incoming_values()) {
      visitValue(v);
    }
    recordInst(&phi);
    return true;
  }

  bool visitSelectInst(SelectInst &select) {
    bool isTarget =
        visitValue(select.getTrueValue()) && visitValue(select.getFalseValue());
    if (isTarget) {
      recordInst(&select);
      visitValue(select.getCondition());
    }
    return isTarget;
  }

  bool visitAllocaInst(AllocaInst &alloca) {
    recordInst(&alloca);
    return true;
  }

  bool visitICmpInst(ICmpInst &icmp) {
    // struct list_head comparison is ignored as we model loop conditions as
    // nondet.
    StructType *listHeadType =
        StructType::getTypeByName(icmp.getContext(), "struct.list_head");

    auto isList = [listHeadType](const Value *v) {
      const Value *base = getUnderlyingObject(v);
      return equivTypes(v->getType(), listHeadType->getPointerTo()) ||
             equivTypes(base->getType(), listHeadType->getPointerTo());
    };

    if (icmp.isEquality() &&
        (isList(icmp.getOperand(0)) || isList(icmp.getOperand(1))))
      return false;

    bool isTarget =
        all_of(icmp.operands(), [this](Value *v) { return visitValue(v); });
    if (isTarget)
      recordInst(&icmp);
    return isTarget;
  }

  bool visitInstruction(Instruction &inst) {
    bool isTarget =
        all_of(inst.operands(), [this](Value *v) { return visitValue(v); });
    if (isTarget) {
      recordInst(&inst);
    }
    return isTarget;
  }

private:
  DenseSet<const CallInst *> refcountGenerators;
  DenseSet<const Argument *> targetArgs;
  DenseSet<const Instruction *> targets;
  DenseSet<const Value *> underlyingTargetPtrs;
  DenseSet<const Value *> underlyingRetPtrs;
  DenseSet<const Value *> underlyingTargetArgsPtrs;
  DenseMap<const Value *, bool> cache;

  bool visitValue(Value *val) {
    if (cache.count(val))
      return cache[val];
    cache[val] = false;
    bool isTarget = false;
    if (Instruction *inst = dyn_cast<Instruction>(val)) {
      isTarget = visit(inst);
    } else if (BasicBlock *blk = dyn_cast<BasicBlock>(val)) {
      isTarget = visit(blk->getTerminator());
    } else if (Constant *c = dyn_cast<Constant>(val)) {
      isTarget = visitConstant(c);
    } else if (isa<Argument>(val)) {
      isTarget = true;
    }
    cache[val] = isTarget;
    return isTarget;
  }

  bool visitConstant(Constant *c) {
    if (GlobalVariable *gv = dyn_cast<GlobalVariable>(c)) {
      return visitGlobalVariable(gv);
    }
    return true;
  }

  bool visitGlobalVariable(GlobalVariable *gv) {
    if (gv->getName().startswith("drvhorn."))
      return true;
    // if the global variable is a poniter, it may be stored somewhere else,
    // thus regard it as nondet.
    Type *t = gv->getValueType();
    if (t->isPointerTy()) {
      return false;
    }
    LLVMContext &ctx = gv->getContext();
    StructType *krefType = StructType::getTypeByName(ctx, "struct.kref");
    PointerType *devPtrType =
        StructType::getTypeByName(ctx, "struct.device")->getPointerTo();
    return embedsStructIndirect(t, krefType) ||
           embedsStructIndirect(t, devPtrType);
  }

  void recordInst(Instruction *inst) {
    targets.insert(inst);
    for (BasicBlock *blk : predecessors(inst->getParent())) {
      visitValue(blk->getTerminator());
    }
  }

  bool isStoreTarget(const StoreInst &store) {
    LLVMContext &ctx = store.getContext();
    StructType *listHeadType =
        StructType::getTypeByName(ctx, "struct.list_head");
    const Value *val = store.getValueOperand();
    // The store of struct list_head is ignored.
    if (equivTypes(val->getType(), listHeadType->getPointerTo()))
      return false;

    const Value *v = getUnderlyingObject(store.getPointerOperand());
    if (StructType *baseType =
            dyn_cast<StructType>(v->getType()->getPointerElementType())) {
      StructType *deviceType = StructType::getTypeByName(ctx, "struct.device");
      if (embedsStruct(baseType, deviceType))
        return false;
    }
    if (const Argument *arg = dyn_cast<Argument>(v)) {
      if (targetArgs.count(arg))
        return true;
      SmallVector<const Value *> underlyingVals;
      getUnderlyingObjects(val, underlyingVals, nullptr, 0);
      return any_of(underlyingVals, [this](const Value *v) {
        return refcountGenerators.count(dyn_cast<CallInst>(v));
      });
    }
    return underlyingTargetPtrs.count(v) || underlyingRetPtrs.count(v) ||
           underlyingTargetArgsPtrs.count(v);
  }

  void recordUnderlyingObjectOfRetVal(ReturnInst *ret) {
    Value *retVal = ret->getReturnValue();
    if (retVal->getType()->isPointerTy()) {
      SmallVector<const Value *> objects;
      getUnderlyingObjects(retVal, objects, nullptr, 0);
      for (const Value *v : objects) {
        underlyingRetPtrs.insert(v);
      }
    }
  }

  DenseSet<const CallInst *> trivialFwnodeGetters(const Module &m) {
    DenseSet<const CallInst *> trivialCalls;
    for (const Function &f : m) {
      for (const BasicBlock &blk : f) {
        const CallInst *target = nullptr;
        for (const Instruction &inst : blk) {
          if (const CallInst *call = dyn_cast<CallInst>(&inst)) {
            const Function *calledFn = extractCalledFunction(call);
            if (!calledFn)
              continue;
            if (calledFn->getName().startswith("drvhorn.fwnode_getter")) {
              if (target)
                errs() << "target drvhorn.fwnode_getter already set\n";
              target = call;
            } else if (calledFn->getName().equals("drvhorn.fwnode_put")) {
              if (target) {
                if (call->getArgOperand(0)->stripPointerCasts() !=
                    target->stripPointerCasts()) {
                  errs() << "drvhorn.fwnode_put target does not match\n";
                }
                trivialCalls.insert(target);
                trivialCalls.insert(call);
              }
            }
          }
        }
      }
    }
    return trivialCalls;
  }

  void recordCallers(const Function *f,
                     const DenseSet<const CallInst *> &ignoreList) {
    DenseSet<const Function *> visited;
    SmallVector<const Function *> workList;
    workList.push_back(f);
    while (!workList.empty()) {
      const Function *f = workList.back();
      workList.pop_back();
      if (!visited.insert(f).second)
        continue;
      for (const CallInst *call : getCalls(f)) {
        if (ignoreList.contains(call))
          continue;
        refcountGenerators.insert(call);
        workList.push_back(call->getFunction());
      }
    }
  }

  void buildGenerators(const Module &m,
                       const DenseSet<const CallInst *> &ignoreList) {
    DenseSet<const Function *> visited;
    SmallVector<const Function *> workList;
    for (const Function &f : m) {
      if (f.hasFnAttribute("drvhorn.checkpoint"))
        workList.push_back(&f);
    }
    while (!workList.empty()) {
      const Function *f = workList.back();
      workList.pop_back();
      if (!visited.insert(f).second)
        continue;
      for (const CallInst *call : getCalls(f)) {
        if (ignoreList.contains(call))
          continue;
        refcountGenerators.insert(call);
        workList.push_back(call->getFunction());
      }
    }
  }

  void buildTargetArgs(Module &m) {
    buildKrefTargetArgs(m);
    for (const Argument *arg : targetArgs) {
      for (const CallInst *call : getCalls(arg->getParent())) {
        if (const Value *v = call->getArgOperand(arg->getArgNo())) {
          SmallVector<const Value *> objs;
          getUnderlyingObjects(v, objs, nullptr, 0);
          for (const Value *obj : objs) {
            underlyingTargetArgsPtrs.insert(obj);
          }
        }
      }
    }
  }

  void buildKrefTargetArgs(Module &m) {
    SmallVector<const Argument *> workList;
    DenseSet<const Argument *> visited;
    StringRef fnNames[] = {
        "drvhorn.kref_init",
        "drvhorn.kref_get",
        "drvhorn.kref_put",
    };
    for (StringRef name : fnNames) {
      if (const Function *f = m.getFunction(name)) {
        const Argument *arg = f->getArg(0);
        workList.push_back(arg);
        visited.insert(arg);
      }
    }
    StructType *devType =
        StructType::getTypeByName(m.getContext(), "struct.device");

    while (!workList.empty()) {
      const Argument *arg = workList.pop_back_val();
      targetArgs.insert(arg);
      for (const CallInst *call : getCalls(arg->getParent())) {
        const Value *v = call->getArgOperand(arg->getArgNo());
        for (const Argument *arg : underlyingArgs(v)) {
          if (visited.insert(arg).second) {
            workList.push_back(arg);
          }
        }

        bool isDevice = equivTypes(v->getType(), devType->getPointerTo());
        if (isDevice) {
          for (const Argument *arg : devPointingArgs(v)) {
            if (visited.insert(arg).second) {
              workList.push_back(arg);
            }
          }
        }
      }
    }
  }

  void collectUnderlyingArgs(const Value *v,
                             SmallVector<const Argument *> &args) {
    SmallVector<const Value *> workList;
    getUnderlyingObjects(v, workList, nullptr, 0);
    DenseSet<const Value *> visited;

    while (!workList.empty()) {
      const Value *v = workList.pop_back_val();
      if (!visited.insert(v).second)
        continue;
      if (const Argument *arg = dyn_cast<Argument>(v)) {
        args.push_back(arg);
      } else if (const LoadInst *load = dyn_cast<LoadInst>(v)) {
        const Value *ptr = load->getPointerOperand();
        for (const User *user : ptr->users()) {
          if (const StoreInst *store = dyn_cast<StoreInst>(user)) {
            const Value *v = store->getValueOperand();
            getUnderlyingObjects(v, workList, nullptr, 0);
          }
        }
      }
    }
  }

  SmallVector<const Argument *> underlyingArgs(const Value *v) {
    SmallVector<const Argument *> args;
    collectUnderlyingArgs(v, args);
    return args;
  }

  SmallVector<const Argument *> devPointingArgs(const Value *devPtr) {
    SmallVector<const Value *> underlyingVals;
    getUnderlyingObjects(devPtr, underlyingVals, nullptr, 0);

    SmallVector<const Argument *> args;
    for (const Value *v : underlyingVals) {
      if (const LoadInst *load = dyn_cast<LoadInst>(v)) {
        collectUnderlyingArgs(load->getPointerOperand(), args);
      }
    }
    return args;
  }
};

class Slicer : public ModulePass {
public:
  static char ID;

  Slicer() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    updateLinkage(m);
    removeCompilerUsed(m);
    runDCEPasses(m, 1);

    sliceModule(m);
    runDCEPasses(m, 10);

    replaceConstIntToPtrToNull(m);
    removeNotCalledFunctions(m);
    runDCEPasses(m, 20);
    return true;
  }

  void getAnalysisUsage(AnalysisUsage &au) const override {
    au.addRequired<seahorn::SeaBuiltinsInfoWrapperPass>();
    au.setPreservesAll();
  }

  virtual StringRef getPassName() const override { return "Slicer"; }

private:
  void removeCompilerUsed(Module &m) {
    if (GlobalVariable *compilerUsed = m.getNamedGlobal(COMPILER_USED_NAME)) {
      Type *ty = compilerUsed->getType();
      compilerUsed->eraseFromParent();
      // llvm.compiler.used seems to be required, so insert an empty value
      m.getOrInsertGlobal(COMPILER_USED_NAME, ty->getPointerElementType());
    }
  }

  void updateLinkage(Module &m) {
    for (Function &f : m) {
      if (f.isDeclaration() || f.getName().equals("main") ||
          f.getName().startswith("__VERIFIER_") ||
          // might be used later
          f.getName().equals("drvhorn.kref_init") ||
          f.getName().equals("drvhorn.alloc"))
        continue;
      f.setLinkage(GlobalValue::InternalLinkage);
    }
    for (GlobalVariable &v : m.globals()) {
      if (v.isDeclaration())
        continue;
      v.setLinkage(GlobalValue::InternalLinkage);
    }
    for (GlobalAlias &alias : m.aliases()) {
      if (alias.isDeclaration())
        continue;
      alias.setLinkage(GlobalValue::InternalLinkage);
    }
  }

  Value *getReplacement(Instruction *inst,
                        DenseMap<const Type *, Function *> &ndvalfn) {
    Instruction *insertPoint =
        isa<PHINode>(inst) ? inst->getParent()->getFirstNonPHI() : inst;
    return nondetValue(inst->getType(), insertPoint, ndvalfn);
  }

  void handleRetainedInst(Instruction *inst,
                          DenseMap<const Type *, Function *> &ndvalfn,
                          const DenseSet<Instruction *> &toRemoveInstructions) {
    for (Value *op : inst->operands()) {
      if (Instruction *opInst = dyn_cast<Instruction>(op)) {
        if (toRemoveInstructions.count(opInst)) {
          Value *replace = getReplacement(opInst, ndvalfn);
          opInst->replaceAllUsesWith(replace);
        }
      }
    }
  }

  void fillWriteOnlyArgs(CallInst *call,
                         const DenseSet<Instruction *> &toRemove,
                         DenseMap<const Type *, Function *> &ndvalfn) {
    Function *f = extractCalledFunction(call);
    if (!f || f->getName().startswith("drvhorn."))
      return;
    StructType *devType =
        StructType::getTypeByName(call->getContext(), "struct.device");
    for (Argument &arg : f->args()) {
      if (!arg.hasAttribute(Attribute::WriteOnly))
        continue;
      Value *argVal = call->getArgOperand(arg.getArgNo())->stripPointerCasts();
      if (Instruction *inst = dyn_cast<Instruction>(argVal)) {
        if (toRemove.count(inst))
          continue;
      }
      if (StructType *s =
              dyn_cast<StructType>(arg.getType()->getPointerElementType())) {
        if (embedsStruct(s, devType))
          continue;
      }
      Value *ndVal = nondetValue(argVal->getType()->getPointerElementType(),
                                 call, ndvalfn);
      ndVal->setName("arg_filler." + call->getName().str());
      IRBuilder<> b(call);
      b.CreateStore(ndVal, argVal);
    }
  }

  bool isNewNondet(const Instruction &inst) {
    return isa<CallInst, LoadInst, StoreInst, InsertElementInst,
               InsertValueInst, ICmpInst>(inst);
  }

  void sliceModule(Module &m) {
    Filter filter(m);
    DenseMap<const Type *, Function *> ndvalfn;
    for (Function &f : m) {
      // we keep these functions still.
      if (f.getName().equals("main") || f.getName().startswith("drvhorn.") ||
          f.isDeclaration())
        continue;
      DenseSet<Instruction *> toRemoveInstructions;
      SmallVector<Instruction *> retained;
      for (Instruction &inst : instructions(f)) {
        if (filter.isTarget(&inst)) {
          retained.push_back(&inst);
        } else {
          if (isNewNondet(inst)) {
            toRemoveInstructions.insert(&inst);
            if (CallInst *call = dyn_cast<CallInst>(&inst))
              fillWriteOnlyArgs(call, toRemoveInstructions, ndvalfn);
          } else {
            retained.push_back(&inst);
          }
        }
      }
      for (Instruction *inst : retained) {
        handleRetainedInst(inst, ndvalfn, toRemoveInstructions);
      }
      for (Instruction *inst : toRemoveInstructions) {
        inst->eraseFromParent();
      }
    }
  }

  // ERR_PTR should be replaced with null
  void replaceConstIntToPtrToNull(Module &m) {
    auto isConstIntToPtr = [](Value *v) {
      ConstantExpr *ce = dyn_cast<ConstantExpr>(v);
      return ce && ce->getOpcode() == Instruction::IntToPtr;
    };

    for (Function &f : m) {
      for (Instruction &inst : instructions(f)) {
        // Instructions like
        // store %struct.list_head*(inttoptr ...), %struct.list_head **ptr
        // should be kept.
        if (isa<StoreInst>(inst))
          continue;
        for (Value *v : inst.operands()) {
          if (isConstIntToPtr(v)) {
            Value *null = Constant::getNullValue(v->getType());
            v->replaceAllUsesWith(null);
          }
        }
      }
    }
  }

  void runDCEPasses(Module &m, unsigned limit) {
    legacy::PassManager pm;
    pm.add(createVerifierPass(false));
    pm.add(createGlobalDCEPass());
    pm.add(createAggressiveDCEPass());
    pm.add(createDeadArgEliminationPass());
    pm.add(createCFGSimplificationPass());
    unsigned counter = 0;
    while (counter++ < limit) {
      removeUnusedNondetCalls(m);
      if (!pm.run(m))
        break;
    }
  }

  void removeNotCalledFunctions(Module &m) {
    for (Function &f : m) {
      if (getCalls(&f).empty() && !f.getName().equals("main") &&
          // functions below might be used later.
          !f.getName().equals("drvhorn.fail") &&
          !f.getName().equals("drvhorn.kref_init"))
        f.deleteBody();
    }
  }

  void removeUnusedNondetCalls(Module &m) {
    // nondet function calls are not removed by DCE passes.
    SmallVector<Instruction *, 16> toRemove;
    SeaBuiltinsInfo &sbi = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    for (Function &f : m) {
      for (Instruction &inst : instructions(f)) {
        if (CallInst *call = dyn_cast<CallInst>(&inst)) {
          if (sbi.isSeaBuiltin(*call)) {
            continue;
          }
          if (Function *f = extractCalledFunction(call)) {
            if (f->isDeclaration() && call->user_empty() &&
                !f->getName().equals("drvhorn.fail") &&
                !f->hasFnAttribute("drvhorn.devm")) {
              toRemove.push_back(call);
            }
          }
        }
      }
    }
    for (Instruction *inst : toRemove) {
      inst->eraseFromParent();
    }
  }

  Value *nondetValue(Type *type, Instruction *before,
                     DenseMap<const Type *, Function *> &ndvalfn) {
    IRBuilder<> b(before);
    Module *m = before->getModule();
    if (!type->isPointerTy()) {
      Function *nondetFn = getNondetValueFn(type, m, ndvalfn);
      Value *call = b.CreateCall(nondetFn);
      if (StructType *s = dyn_cast<StructType>(type)) {
        for (unsigned i = 0; i < s->getNumElements(); i++) {
          Value *elem = nondetValue(s->getElementType(i), before, ndvalfn);
          call = b.CreateInsertValue(call, elem, i);
        }
      }
      return call;
    }

    Type *elemType = type->getPointerElementType();
    if (FunctionType *ft = dyn_cast<FunctionType>(elemType)) {
      return makeNewValFn(m, ft, ndvalfn.size());
    }
    if (StructType *st = dyn_cast<StructType>(elemType)) {
      if (st->isOpaque()) {
        Function *f = getNondetValueFn(type, m, ndvalfn);
        return b.CreateCall(f);
      }
    }
    Value *call;
    if (isNullChecked(before)) {
      Function *alloc = getOrCreateAlloc(*m);
      DataLayout dl(m);
      uint64_t size;
      if (elemType->isIntegerTy(8))
        // for i8*, we allocate 4096 bytes as this allocation might be
        // kmalloc().
        // TODO: the size should be calculated correctly.
        size = 4096;
      else
        size = dl.getTypeAllocSize(elemType);
      Value *p = b.CreateCall(alloc, b.getInt64(size));
      call = b.CreateBitCast(p, type);
    } else {
      call = b.CreateAlloca(elemType);
    }
    if (elemType->isPointerTy()) {
      Value *value = nondetValue(elemType, before, ndvalfn);
      b.CreateStore(value, call);
    }
    return call;
  }

  bool isNullChecked(const Instruction *before) {
    auto isNull = [](const Value *v) {
      if (const Constant *c = dyn_cast<Constant>(v)) {
        return c->isNullValue();
      }
      return false;
    };

    SmallVector<const User *> workList;
    DenseSet<const User *> visited;
    for (const User *user : before->users()) {
      workList.push_back(user);
      visited.insert(user);
    }
    while (!workList.empty()) {
      const User *user = workList.pop_back_val();
      if (const ICmpInst *cmp = dyn_cast<ICmpInst>(user)) {
        if (cmp->getPredicate() == CmpInst::ICMP_EQ) {
          if (isNull(cmp->getOperand(0)) || isNull(cmp->getOperand(1))) {
            return true;
          }
        }
      }
      if (isa<BitCastOperator>(user)) {
        for (const User *user : user->users()) {
          if (visited.insert(user).second)
            workList.push_back(user);
        }
      }
    }
    return false;
  }

  Function *getNondetValueFn(Type *retType, Module *m,
                             DenseMap<const Type *, Function *> &ndvalfn) {
    auto it = ndvalfn.find(retType);
    if (it != ndvalfn.end()) {
      return it->second;
    }
    Function *res =
        makeNewValFn(m, retType, ndvalfn.size(), "verifier.nondetvalue.");
    ndvalfn[retType] = res;
    return res;
  }

  Function *makeNewValFn(Module *m, Type *type, unsigned startFrom,
                         std::string prefix) {
    std::string name;
    unsigned c = startFrom;
    do {
      name = prefix + std::to_string(c++);
    } while (m->getNamedValue(name));
    return Function::Create(FunctionType::get(type, false),
                            GlobalValue::ExternalLinkage, name, m);
  }

  Function *makeNewValFn(Module *m, FunctionType *type, unsigned startFrom) {
    std::string name;
    unsigned c = startFrom;
    do {
      name = "verifier.nondetvaluefn." + std::to_string(c++);
    } while (m->getNamedValue(name));
    return Function::Create(type, GlobalValue::ExternalLinkage, name, m);
  }
};

char Slicer::ID = 0;
Pass *createSlicerPass() { return new Slicer(); }
} // namespace seahorn
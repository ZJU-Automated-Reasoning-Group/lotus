#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Transforms/Kernel/SetupEntrypoint.hh"
#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

static unsigned fileOpOpenIndex = 13;

class FileOperations : public ModulePass {
public:
  static char ID;

  FileOperations(StringRef name) : ModulePass(ID), name(name) {}

  bool runOnModule(Module &m) override {
    Function *open = getOpenFunc(m);
    if (!open) {
      errs() << "No open function found for struct file_operations " << name
             << "\n";
      return false;
    }
    constructMain(m, open);
    return true;
  }

  virtual StringRef getPassName() const override { return "FileOperations"; }

private:
  StringRef name;
  DenseMap<const Type *, Function *> ndfn;

  Function *getOpenFunc(Module &m) {
    GlobalVariable *fileOp = m.getNamedGlobal(name);
    Constant *open =
        fileOp->getInitializer()->getAggregateElement(fileOpOpenIndex);
    return dyn_cast_or_null<Function>(open);
  }

  void constructMain(Module &m, Function *open) {
    LLVMContext &ctx = m.getContext();
    Type *i32Ty = Type::getInt32Ty(ctx);
    Function *main = Function::Create(
        FunctionType::get(i32Ty, false),
        GlobalValue::LinkageTypes::ExternalLinkage, "main", &m);
    BasicBlock *entry = BasicBlock::Create(ctx, "entry", main);
    BasicBlock *fail = BasicBlock::Create(ctx, "fail", main);
    BasicBlock *ret = BasicBlock::Create(ctx, "ret", main);

    Value *instance = buildEntryBlock(m, open, entry, fail, ret);
    buildFailBlock(m, fail, ret, instance);
    buildRetBlock(m, ret);
  }

  Value *buildEntryBlock(Module &m, Function *open, BasicBlock *entry,
                         BasicBlock *fail, BasicBlock *ret) {
    IRBuilder<> b(entry);
    Type *i32Ty = Type::getInt32Ty(m.getContext());

    Argument *inodeArg = open->getArg(0);
    Type *inodePtrType = inodeArg->getType();
    Type *inodeType = inodePtrType->getPointerElementType();
    unsigned iPrivateFieldIdx = inodeType->getStructNumElements() - 1;
    const DenseMap<uint64_t, Type *> &fields =
        iPrivateFields(inodeArg, iPrivateFieldIdx);
    Value *inode = allocType(m, b, inodeType);
    size_t byteSize = getIPrivateSize(m, fields);
    Value *iPrivate =
        buildIPrivate(m, b, inodeType, inode, byteSize, iPrivateFieldIdx);
    populateFields(m, b, iPrivate, fields);
    Function *krefInit = m.getFunction("drvhorn.kref_init");
    Type *krefType = krefInit->getArg(0)->getType()->getPointerElementType();
    GlobalVariable *globalKref = new GlobalVariable(
        m, krefType->getPointerTo(), false, GlobalValue::PrivateLinkage,
        ConstantPointerNull::get(krefType->getPointerTo()),
        "drvhorn.kref.struct.file_operations");
    SmallVector<Value *, 8> krefPtrs =
        embeddedKrefPtrs(m, b, iPrivate, fields, krefType);
    switch (krefPtrs.size()) {
    case 0:
      errs() << "No kref found\n";
      break;
    case 1:
      b.CreateCall(krefInit, krefPtrs[0]);
      b.CreateStore(krefPtrs[0], globalKref);
      break;
    default:
      errs() << "TODO: multiple kref\n";
      return nullptr;
    }

    Type *filePtrType = open->getArg(1)->getType();
    Value *file = allocType(m, b, filePtrType->getPointerElementType());
    CallInst *call = b.CreateCall(open->getFunctionType(), open, {inode, file});
    Value *notZero = b.CreateICmpNE(call, ConstantInt::get(i32Ty, 0));
    b.CreateCondBr(notZero, fail, ret);
    return file;
  }

  SmallVector<const Value *> getIPrivatePtrs(const Argument *arg,
                                             unsigned iPrivateFieldIdx) {
    SmallVector<const GEPOperator *> geps;
    for (const User *user : arg->users()) {
      if (const GEPOperator *gep = dyn_cast<GEPOperator>(user)) {
        if (gep->getNumIndices() != 2)
          continue;
        ConstantInt *idx = dyn_cast<ConstantInt>(gep->getOperand(2));
        if (!idx)
          continue;
        if (idx->getZExtValue() == iPrivateFieldIdx) {
          geps.push_back(gep);
        }
      }
    }
    SmallVector<const Value *> res;
    for (const GEPOperator *gep : geps) {
      for (const User *user : gep->users()) {
        if (const LoadInst *load = dyn_cast<LoadInst>(user)) {
          res.push_back(load);
        }
      }
    }
    return res;
  }

  Type *getActualIPrivateFieldType(const GEPOperator *fieldPtr) {
    for (const User *user : fieldPtr->users()) {
      if (const BitCastOperator *bitcast = dyn_cast<BitCastOperator>(user)) {
        return bitcast->getDestTy()->getPointerElementType();
      }
    }
    return nullptr;
  }

  DenseMap<uint64_t, Type *> iPrivateFields(const Argument *inodeArg,
                                            unsigned iPrivateFieldIdx) {
    const SmallVector<const Value *> &iPrivatePtrs =
        getIPrivatePtrs(inodeArg, iPrivateFieldIdx);
    DenseMap<uint64_t, Type *> fields;
    for (const Value *iPrivatePtr : iPrivatePtrs) {
      for (const User *user : iPrivatePtr->users()) {
        if (const BitCastOperator *bitcast = dyn_cast<BitCastOperator>(user)) {
          // if bitcasted before GEP, the dest type should be the type at the
          // 0th index.
          fields[0] = bitcast->getDestTy()->getPointerElementType();
        } else if (const GEPOperator *gep = dyn_cast<GEPOperator>(user)) {
          if (gep->getNumIndices() != 1)
            continue;
          ConstantInt *idx = dyn_cast<ConstantInt>(gep->getOperand(1));
          if (!idx)
            continue;
          uint64_t fieldIdx = idx->getZExtValue();
          if (Type *t = getActualIPrivateFieldType(gep))
            fields[fieldIdx] = t;
        }
      }
    }
    return fields;
  }

  Value *allocType(Module &m, IRBuilder<> &b, Type *type) {
    AllocaInst *alloc = b.CreateAlloca(type);
    Function *nondet = getNondetFn(type, m);
    CallInst *val = b.CreateCall(nondet);
    // Fill the allocated memory with a nondet value.
    b.CreateStore(val, alloc);
    return alloc;
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

  size_t getIPrivateSize(const Module &m,
                         const DenseMap<uint64_t, Type *> &fields) {
    size_t byteSize = 0;
    const DataLayout &dl = m.getDataLayout();
    for (const auto &field : fields) {
      byteSize += dl.getTypeAllocSize(field.second);
    }
    return byteSize;
  }

  Value *buildIPrivate(Module &m, IRBuilder<> &b, Type *inodeType,
                       Value *inodePtr, uint64_t byteSize,
                       unsigned iPrivateFieldIdx) {
    Type *i8Type = Type::getInt8Ty(m.getContext());
    Type *i32Type = Type::getInt32Ty(m.getContext());
    Type *i64Type = Type::getInt64Ty(m.getContext());
    Constant *zero = ConstantInt::get(i64Type, 0);
    Constant *iPrivateOffset = ConstantInt::get(i32Type, iPrivateFieldIdx);
    Value *iPrivateGEP =
        b.CreateGEP(inodeType, inodePtr, {zero, iPrivateOffset});
    Value *iPrivatePtr =
        b.CreateAlloca(i8Type, ConstantInt::get(i64Type, byteSize));
    b.CreateStore(iPrivatePtr, iPrivateGEP);
    return iPrivatePtr;
  }

  SmallVector<Value *, 8>
  embeddedKrefPtrs(Module &m, IRBuilder<> &b, Value *iPrivate,
                   const DenseMap<uint64_t, Type *> &fields, Type *krefType) {
    SmallVector<Value *, 8> devicePtrs;
    LLVMContext &ctx = m.getContext();
    Type *i8Type = Type::getInt8Ty(ctx);
    Type *i32Type = Type::getInt32Ty(ctx);
    Type *i64Type = Type::getInt64Ty(ctx);
    for (const auto &field : fields) {
      uint64_t idx = field.first;
      Type *fieldType = field.second;
      if (!fieldType->isPointerTy())
        continue;
      StructType *structType =
          dyn_cast<StructType>(fieldType->getPointerElementType());
      if (!structType)
        continue;
      Value *fieldPtrAddr =
          b.CreateGEP(i8Type, iPrivate, ConstantInt::get(i64Type, idx));
      Value *fieldPtr =
          b.CreateBitCast(fieldPtrAddr, fieldType->getPointerTo());
      Value *fieldValue = b.CreateLoad(fieldType, fieldPtr);

      SmallVector<uint64_t, 8> indices =
          indicesToKrefType(structType, krefType);
      if (indices.empty())
        continue;
      SmallVector<Value *, 8> gepIndices;
      gepIndices.push_back(ConstantInt::get(i64Type, 0));
      for (int i = indices.size() - 1; i >= 0; i--) {
        gepIndices.push_back(ConstantInt::get(i32Type, indices[i]));
      }

      Value *gep = b.CreateGEP(structType, fieldValue, gepIndices);
      devicePtrs.push_back(gep);
    }
    return devicePtrs;
  }

  SmallVector<uint64_t, 8> indicesToKrefType(const StructType *type,
                                             Type *krefType) {
    for (size_t i = 0; i < type->getNumElements(); i++) {
      const StructType *fieldType =
          dyn_cast<StructType>(type->getElementType(i));
      if (!fieldType)
        continue;
      if (equivTypes(fieldType, krefType)) {
        return {i};
      }
      SmallVector<uint64_t, 8> indices = indicesToKrefType(fieldType, krefType);
      if (!indices.empty()) {
        indices.push_back(i);
        return indices;
      }
    }
    return {};
  }

  void populateFields(Module &m, IRBuilder<> &b, Value *instanceAddr,
                      const DenseMap<uint64_t, Type *> &fields) {
    LLVMContext &ctx = m.getContext();
    Type *i8Type = Type::getInt8Ty(ctx);
    Type *i64Type = Type::getInt64Ty(ctx);
    for (const auto &field : fields) {
      if (!field.second->isPointerTy())
        continue;
      Type *fieldElemType = field.second->getPointerElementType();
      if (isa<FunctionType>(fieldElemType))
        continue;
      Value *idx = ConstantInt::get(i64Type, field.first);
      Value *fieldAddr = b.CreateGEP(i8Type, instanceAddr, idx);
      Value *fieldPtrDest =
          b.CreateBitCast(fieldAddr, i8Type->getPointerTo()->getPointerTo());
      Value *fieldPtr = allocType(m, b, fieldElemType);
      Value *casted = b.CreateBitCast(fieldPtr, i8Type->getPointerTo());
      b.CreateStore(casted, fieldPtrDest);
    }
  }
};

char FileOperations::ID = 0;

Pass *createFileOperationsSetupPass(StringRef name) {
  return new FileOperations(name);
}
} // namespace seahorn
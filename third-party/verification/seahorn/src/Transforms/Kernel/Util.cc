#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {
StringRef getStructName(StringRef name) {
  size_t p = name.find('.');
  if (p == StringRef::npos)
    return name;
  size_t q = name.find('.', p + 1);
  if (q == StringRef::npos)
    return name;
  return name.substr(0, q);
}

static bool equivTypes(const Type *t1, const Type *t2,
                       DenseSet<const Type *> &visited);

static bool equivTypes(const ArrayType *at1, const ArrayType *at2,
                       DenseSet<const Type *> &visited) {
  return at1->getNumElements() == at2->getNumElements() &&
         equivTypes(at1->getElementType(), at2->getElementType(), visited);
}

static bool equivTypes(const FunctionType *ft1, const FunctionType *ft2,
                       DenseSet<const Type *> &visited) {
  if (ft1->getNumParams() != ft2->getNumParams())
    return false;
  if (!equivTypes(ft1->getReturnType(), ft2->getReturnType(), visited))
    return false;
  for (unsigned i = 0; i < ft1->getNumParams(); i++) {
    if (!equivTypes(ft1->getParamType(i), ft2->getParamType(i), visited))
      return false;
  }
  return true;
}

static bool equivTypes(const IntegerType *it1, const IntegerType *it2) {
  return it1->getBitWidth() == it2->getBitWidth();
}

static bool equivTypes(const PointerType *pt1, const PointerType *pt2,
                       DenseSet<const Type *> &visited) {
  return equivTypes(pt1->getElementType(), pt2->getElementType(), visited);
}

static bool equivTypes(const StructType *st1, const StructType *st2,
                       DenseSet<const Type *> &visited) {
  if (!st1->hasName() || !st2->hasName()) {
    if (st1->getNumElements() != st2->getNumElements())
      return false;
    for (unsigned i = 0; i < st1->getNumElements(); i++) {
      if (!equivTypes(st1->getElementType(i), st2->getElementType(i), visited))
        return false;
    }
    return true;
  }
  StringRef p1 = getStructName(st1->getName());
  StringRef p2 = getStructName(st2->getName());
  return p1 == p2;
}

static bool equivTypes(const VectorType *vt1, const VectorType *vt2,
                       DenseSet<const Type *> &visited) {
  return vt1->getElementCount() == vt2->getElementCount() &&
         equivTypes(vt1->getElementType(), vt2->getElementType(), visited);
}

static bool equivTypes(const Type *t1, const Type *t2,
                       DenseSet<const Type *> &visited) {
  if (!visited.insert(t1).second)
    return true;
  if (isa<ArrayType>(t1) && isa<ArrayType>(t2)) {
    return equivTypes(cast<ArrayType>(t1), cast<ArrayType>(t2), visited);
  }
  if (isa<FunctionType>(t1) && isa<FunctionType>(t2)) {
    return equivTypes(cast<FunctionType>(t1), cast<FunctionType>(t2), visited);
  }
  if (isa<IntegerType>(t1) && isa<IntegerType>(t2)) {
    return equivTypes(cast<IntegerType>(t1), cast<IntegerType>(t2));
  }
  if (isa<PointerType>(t1) && isa<PointerType>(t2)) {
    return equivTypes(cast<PointerType>(t1), cast<PointerType>(t2), visited);
  }
  if (isa<StructType>(t1) && isa<StructType>(t2)) {
    return equivTypes(cast<StructType>(t1), cast<StructType>(t2), visited);
  }
  if (isa<VectorType>(t1) && isa<VectorType>(t2)) {
    return equivTypes(cast<VectorType>(t1), cast<VectorType>(t2), visited);
  }
  return false;
}

bool equivTypes(const Type *t1, const Type *t2) {
  DenseSet<const Type *> visited;
  return equivTypes(t1, t2, visited);
}

const Function *extractCalledFunction(const CallInst *call) {
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

Function *extractCalledFunction(CallInst *call) {
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

const Function *extractCalledFunction(const CallInst &call) {
  return dyn_cast<Function>(call.getCalledOperand()->stripPointerCasts());
}

Function *extractCalledFunction(CallInst &call) {
  return dyn_cast<Function>(call.getCalledOperand()->stripPointerCasts());
}

SmallVector<CallInst *, 16> getCalls(Function *fn) {
  SmallVector<CallInst *, 16> res;
  DenseSet<User *> visited;
  SmallVector<User *> workList;
  workList.push_back(fn);
  while (!workList.empty()) {
    User *user = workList.pop_back_val();
    if (!visited.insert(user).second)
      continue;
    if (CallInst *call = dyn_cast<CallInst>(user)) {
      res.push_back(call);
    } else if (isa<BitCastOperator, Function>(user)) {
      for (User *user : user->users()) {
        workList.push_back(user);
      }
    }
  }
  return res;
}

SmallVector<const CallInst *, 16> getCalls(const Function *fn) {
  SmallVector<const CallInst *, 16> res;
  DenseSet<const User *> visited;
  SmallVector<const User *> workList;
  workList.push_back(fn);
  while (!workList.empty()) {
    const User *user = workList.pop_back_val();
    if (!visited.insert(user).second)
      continue;
    if (const CallInst *call = dyn_cast<CallInst>(user)) {
      res.push_back(call);
    } else if (isa<BitCastOperator, Function>(user)) {
      for (const User *user : user->users()) {
        workList.push_back(user);
      }
    }
  }
  return res;
}

Function *getOrCreateNdIntFn(Module &m, unsigned bitwidth) {
  std::string name = "nd.int" + std::to_string(bitwidth);
  if (Function *f = m.getFunction(name))
    return f;
  IntegerType *it = IntegerType::get(m.getContext(), bitwidth);
  FunctionType *ft = FunctionType::get(it, false);
  return Function::Create(ft, Function::ExternalLinkage, name, &m);
}

Function *getOrCreateAlloc(Module &m) {
  if (Function *f = m.getFunction("drvhorn.alloc"))
    return f;
  LLVMContext &ctx = m.getContext();
  Function *ndBool = getOrCreateNdIntFn(m, 1);
  IntegerType *i8Ty = Type::getInt8Ty(ctx);
  IntegerType *i64Ty = Type::getInt64Ty(ctx);
  Function *f =
      Function::Create(FunctionType::get(i8Ty->getPointerTo(), i64Ty, false),
                       GlobalValue::ExternalLinkage, "drvhorn.alloc", &m);
  Argument *size = f->getArg(0);
  BasicBlock *blk = BasicBlock::Create(ctx, "", f);
  IRBuilder<> b(blk);
  Value *cond = b.CreateCall(ndBool, {}, "alloc.cond");
  AllocaInst *alloca = b.CreateAlloca(i8Ty, size);
  Value *result = b.CreateSelect(
      cond, alloca, ConstantPointerNull::get(i8Ty->getPointerTo()));
  b.CreateRet(result);
  return f;
}

static Optional<SmallVector<Value *>>
revGEPIndicesToStruct(const StructType *s, const Type *target) {
  IntegerType *i32Ty = Type::getInt32Ty(s->getContext());
  for (unsigned i = 0; i < s->getNumElements(); i++) {
    const Type *elemType = s->getElementType(i);
    if (equivTypes(elemType, target))
      return SmallVector<Value *>{ConstantInt::get(i32Ty, i)};
    if (const StructType *sTy = dyn_cast<StructType>(elemType)) {
      Optional<SmallVector<Value *>> indices =
          revGEPIndicesToStruct(sTy, target);
      if (indices.hasValue()) {
        indices->push_back(ConstantInt::get(i32Ty, i));
        return indices;
      }
    }
  }
  return None;
}

Optional<SmallVector<Value *>> gepIndicesToStruct(const StructType *s,
                                                  const Type *target) {
  IntegerType *i64Ty = Type::getInt64Ty(s->getContext());
  if (equivTypes(s, target)) {
    return SmallVector<Value *>{ConstantInt::get(i64Ty, 0)};
  }
  Optional<SmallVector<Value *>> indices = revGEPIndicesToStruct(s, target);
  if (!indices.hasValue())
    return None;
  indices->push_back(ConstantInt::get(i64Ty, 0));
  return SmallVector<Value *>(indices->rbegin(), indices->rend());
}

Type *getGEPType(StructType *s, ArrayRef<Value *> indices) {
  indices = indices.drop_front();
  Type *cur = s;
  for (Value *index : indices) {
    ConstantInt *c = dyn_cast<ConstantInt>(index);
    if (!c)
      return nullptr;
    StructType *s = dyn_cast<StructType>(cur);
    if (!s)
      return nullptr;
    cur = s->getElementType(c->getZExtValue());
  }
  return cur;
}

bool embedsStruct(const StructType *s, const Type *target) {
  if (equivTypes(s, target))
    return true;
  for (unsigned i = 0; i < s->getNumElements(); i++) {
    const Type *elemType = s->getElementType(i);
    if (equivTypes(elemType, target))
      return true;
    if (const StructType *sTy = dyn_cast<StructType>(elemType)) {
      if (embedsStruct(sTy, target))
        return true;
    }
  }
  return false;
}
} // namespace seahorn
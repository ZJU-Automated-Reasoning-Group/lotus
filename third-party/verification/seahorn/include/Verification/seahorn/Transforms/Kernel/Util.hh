#pragma once

#include "llvm/IR/Instructions.h"

namespace seahorn {
#define FWNODE_REFCOUNT_INDEX 5
#define DEVPMINFO_WAKEUP_INDEX 1

llvm::StringRef getStructName(llvm::StringRef name);
bool equivTypes(const llvm::Type *t1, const llvm::Type *t2);
const llvm::Function *extractCalledFunction(const llvm::CallInst *call);
llvm::Function *extractCalledFunction(llvm::CallInst *call);
const llvm::Function *extractCalledFunction(const llvm::CallInst &call);
llvm::Function *extractCalledFunction(llvm::CallInst &call);
llvm::SmallVector<llvm::CallInst *, 16> getCalls(llvm::Function *fn);
llvm::SmallVector<const llvm::CallInst *, 16>
getCalls(const llvm::Function *fn);
llvm::Function *getOrCreateNdIntFn(llvm::Module &m, unsigned);
llvm::Function *getOrCreateAlloc(llvm::Module &m);
llvm::Optional<llvm::SmallVector<llvm::Value *>>
gepIndicesToStruct(const llvm::StructType *s, const llvm::Type *target);
llvm::Type *getGEPType(llvm::StructType *s,
                       llvm::ArrayRef<llvm::Value *> indices);
bool embedsStruct(const llvm::StructType *s, const llvm::Type *target);
} // namespace seahorn
#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

namespace seahorn {
void setupDevicePtr(llvm::Module &m, llvm::IRBuilder<> &b, llvm::Value *devPtr);
void buildFailBlock(llvm::Module &m, llvm::BasicBlock *fail,
                    llvm::BasicBlock *ret, llvm::Value *instance);
void buildRetBlock(llvm::Module &m, llvm::BasicBlock *ret);
}; // namespace seahorn
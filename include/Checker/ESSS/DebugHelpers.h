#pragma once

#include <llvm/Pass.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/CFG.h>
#include "PathSpan.h"

using namespace llvm;

void dumpDebugValue(const Function* function);
std::string getBasicBlockName(const BasicBlock* BB);
void dumpPathList(esss::span<const BasicBlock* const> blocks);

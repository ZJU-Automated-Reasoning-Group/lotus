#ifndef __SORT_TOPO_HH_
#define __SORT_TOPO_HH_

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Function.h"

using namespace llvm;

void RevTopoSort(const llvm::Function &F, std::vector<const BasicBlock *> &out);

#endif
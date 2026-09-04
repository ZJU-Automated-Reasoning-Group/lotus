#pragma once

#include "llvm/IR/Module.h"

namespace seahorn {
void slimDown(llvm::Module &M, llvm::User *root);
} // namespace seahorn
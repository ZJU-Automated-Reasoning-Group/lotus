#pragma once

#include <llvm/Support/CommandLine.h>

namespace lotus::checker::tooling {

llvm::cl::SubCommand &genericSubCommand();
llvm::cl::SubCommand &kintSubCommand();
llvm::cl::SubCommand &taintSubCommand();
llvm::cl::SubCommand &concurrencySubCommand();
llvm::cl::SubCommand &pulseSubCommand();
llvm::cl::SubCommand &fitxSubCommand();
llvm::cl::SubCommand &saberSubCommand();
llvm::cl::SubCommand &aeSubCommand();
llvm::cl::SubCommand &symexSubCommand();

} // namespace lotus::checker::tooling

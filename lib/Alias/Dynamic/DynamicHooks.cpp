#include "Alias/Dynamic/DynamicHooks.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

using namespace llvm;

namespace dynamic {

namespace {

/// Helper functions to create LLVM types for hook function signatures
Type *getVoidType(const Module &m) { return Type::getVoidTy(m.getContext()); }

Type *getCharType(const Module &m) { return Type::getInt8Ty(m.getContext()); }

Type *getIntType(const Module &m) {
  return Type::getIntNTy(m.getContext(), sizeof(int) * 8);
}

Type *getCharPtrType(const Module &m) {
  return PointerType::getUnqual(getCharType(m));
}

Type *getCharPtrPtrType(const Module &m) {
  return PointerType::getUnqual(getCharPtrType(m));
}

/// Creates an external function declaration with the given name and argument
/// types
Function *createFunctionWithArgType(const StringRef &name,
                                    ArrayRef<Type *> argTypes, Module &module) {
  auto *funType =
      FunctionType::get(getVoidType(module), std::move(argTypes), false);
  return Function::Create(funType, GlobalValue::ExternalLinkage, name, &module);
}
} // namespace

/// Creates all hook function declarations that will be called during execution.
/// These hooks are implemented in MemoryHooks.c and log runtime events.
DynamicHooks::DynamicHooks(Module &module) {
  initHook = createFunctionWithArgType("HookInit", {}, module);
  allocHook = createFunctionWithArgType(
      "HookAlloc",
      {getCharType(module), getIntType(module), getCharPtrType(module)},
      module);
  pointerHook = createFunctionWithArgType(
      "HookPointer", {getIntType(module), getCharPtrType(module)}, module);
  callHook =
      createFunctionWithArgType("HookCall", {getIntType(module)}, module);
  enterHook =
      createFunctionWithArgType("HookEnter", {getIntType(module)}, module);
  exitHook =
      createFunctionWithArgType("HookExit", {getIntType(module)}, module);
  globalHook = createFunctionWithArgType("HookGlobal", {}, module);
  mainHook =
      createFunctionWithArgType("HookMain",
                                {getIntType(module), getCharPtrPtrType(module),
                                 getIntType(module), getCharPtrPtrType(module)},
                                module);
}

/// Checks if a function is one of the instrumentation hooks
bool DynamicHooks::isHook(const llvm::Function &f) const {
  return &f == initHook || &f == allocHook || &f == pointerHook ||
         &f == callHook || &f == enterHook || &f == exitHook ||
         &f == globalHook || &f == mainHook;
}
} // namespace dynamic
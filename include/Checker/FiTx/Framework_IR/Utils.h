#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"

#include <memory>

namespace ir_generator {
template <class Source, class Target>
Target convert(llvm::Instruction *instruction) {
  auto source = llvm::cast<Source>(instruction);
  return Target(source);
}

template <class Source, class Target>
std::shared_ptr<Target> createSharedPtr(llvm::Instruction *instruction) {
  auto source = llvm::cast<Source>(instruction);
  return std::make_shared<Target>(source);
}

int getPointerDereferenceNum(llvm::Type *type);
} // namespace ir_generator

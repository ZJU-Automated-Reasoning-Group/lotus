// Implementation of TypeCollector.
//
// The TypeCollector is responsible for scanning the entire LLVM Module to
// gather the set of all Types that are relevant to the pointer analysis.
//
// Relevance:
// - It visits Global Variables, Functions, and Instructions.
// - It drills down into aggregate types (Structs, Arrays, Pointers).
// - It ignores non-relevant types like `void` or `vector` (partially).
//
// Output:
// - A `TypeSet` containing all unique types found. This set is used by
// subsequent
//   phases (ArrayLayoutAnalysis, PointerLayoutAnalysis) to build type metadata.

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeCollector.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

namespace {

class TypeSetBuilder {
private:
  const Module &module;
  TypeSet &typeSet;

  DenseSet<const Value *> visitedValues;

  void incorporateFunctionType(FunctionType *);
  void incorporateStructType(StructType *);
  void incorporateArrayType(ArrayType *);
  void incorporatePointerType(PointerType *);
  void incorporateType(Type *);

  void incorporateConstant(const Constant *);
  void incorporateInstruction(const Instruction *);
  void incorporateValue(const Value *);

public:
  TypeSetBuilder(const Module &m, TypeSet &t) : module(m), typeSet(t) {}

  void collectType();
};

void TypeSetBuilder::incorporateConstant(const Constant *constant) {
  // Skip global value (handled separately as they have their own types)
  if (isa<GlobalValue>(constant))
    return;

  // Already visited?
  if (!visitedValues.insert(constant).second)
    return;

  // Check the type of the constant itself
  incorporateType(constant->getType());

  // Recursively look in operands for types (e.g. ConstantStruct fields)
  for (auto const &op : constant->operands())
    incorporateValue(op);
}

void TypeSetBuilder::incorporateInstruction(const Instruction *inst) {
  // Already visited?
  if (!visitedValues.insert(inst).second)
    return;

  // Check the return type of the instruction
  incorporateType(inst->getType());

  // Special handling for Alloca: we need the type *being allocated*,
  // which is distinct from the instruction type (pointer to it).
  if (const auto *allocInst = dyn_cast<AllocaInst>(inst))
    incorporateType(allocInst->getAllocatedType());

  // Look in operands for types.
  for (auto const &op : inst->operands()) {
    // Optimization: Skip instruction operands here because they will be visited
    // when iterating the basic block. Only recurse for Constants.
    if (!isa<Instruction>(op))
      incorporateValue(op);
  }
}

// Dispatch based on Value kind.
void TypeSetBuilder::incorporateValue(const Value *value) {
  if (const auto *constant = dyn_cast<Constant>(value))
    incorporateConstant(constant);
  else if (const auto *inst = dyn_cast<Instruction>(value))
    incorporateInstruction(inst);
}

// --- Recursive Type Decomposition ---

void TypeSetBuilder::incorporateFunctionType(FunctionType *funType) {
  for (auto *pType : funType->params())
    incorporateType(pType);
}

void TypeSetBuilder::incorporateStructType(StructType *stType) {
  for (auto *elemType : stType->elements())
    incorporateType(elemType);
}

void TypeSetBuilder::incorporateArrayType(ArrayType *arrayType) {
  incorporateType(arrayType->getElementType());
}

void TypeSetBuilder::incorporatePointerType(PointerType *ptrType) {
  incorporateType(ptrType->getElementType());
}

// Main type insertion logic.
// Decomposes composite types to ensure all sub-types are registered.
void TypeSetBuilder::incorporateType(Type *llvmType) {
  // We don't care about void type
  if (llvmType->isVoidTy())
    return;

  // Check to see if we've already visited this type.
  if (!typeSet.insert(llvmType))
    return;

  if (auto *ptrType = dyn_cast<PointerType>(llvmType))
    incorporatePointerType(ptrType);
  else if (auto *funType = dyn_cast<FunctionType>(llvmType))
    incorporateFunctionType(funType);
  else if (auto *stType = dyn_cast<StructType>(llvmType))
    incorporateStructType(stType);
  else if (auto *arrType = dyn_cast<ArrayType>(llvmType))
    incorporateArrayType(arrType);
  else if (auto *vecType = dyn_cast<VectorType>(llvmType))
    // Treat vectors like arrays: register the element type so pointers inside
    // vectors are still tracked, without requiring full vector support.
    incorporateType(vecType->getElementType());
}

void TypeSetBuilder::collectType() {
  // Get types from global variables
  for (auto const &global : module.globals()) {
    incorporateType(global.getType());
    if (global.hasInitializer())
      incorporateValue(global.getInitializer());
  }

  // Get types from functions
  for (auto const &f : module) {
    assert(!f.hasPrefixData() && !f.hasPrologueData());

    incorporateType(f.getType());

    for (auto const &bb : f)
      for (auto const &inst : bb)
        incorporateValue(&inst);
  }
}

} // namespace

TypeSet TypeCollector::runOnModule(const Module &module) {
  TypeSet typeSet(module);

  TypeSetBuilder(module, typeSet).collectType();

  return typeSet;
}

} // namespace tpa

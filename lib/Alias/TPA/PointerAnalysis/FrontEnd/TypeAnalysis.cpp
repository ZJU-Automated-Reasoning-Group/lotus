// Implementation of TypeAnalysis.
//
// The central type analysis engine.
// Orchestrates the collection and analysis of type information for the entire
// module.
//
// Process:
// 1. TypeCollector: Gather all types.
// 2. StructCastAnalysis: Find compatible structs (bitcasts).
// 3. ArrayLayoutAnalysis: Build array layouts.
// 4. PointerLayoutAnalysis: Build pointer layouts (propagating via casts).
// 5. Build final TypeMap: Combine size, array, and pointer layouts into
// `TypeLayout` objects.

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeAnalysis.h"

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/ArrayLayoutAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/PointerLayoutAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/StructCastAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeCollector.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

namespace {

class TypeMapBuilder {
private:
  const Module &module;
  TypeMap &typeMap;

  size_t getTypeSize(Type *, const DataLayout &);
  void insertTypeMap(Type *, size_t, const ArrayLayout *,
                     const PointerLayout *);
  void insertOpaqueType(Type *);

public:
  TypeMapBuilder(const Module &m, TypeMap &t) : module(m), typeMap(t) {}

  void buildTypeMap();
};

void TypeMapBuilder::insertOpaqueType(Type *type) {
  typeMap.insert(type, TypeLayout::getByteArrayTypeLayout());
}

void TypeMapBuilder::insertTypeMap(Type *type, size_t size,
                                   const ArrayLayout *arrayLayout,
                                   const PointerLayout *ptrLayout) {
  const auto *typeLayout =
      TypeLayout::getTypeLayout(size, arrayLayout, ptrLayout);
  typeMap.insert(type, typeLayout);
}

size_t TypeMapBuilder::getTypeSize(Type *type, const DataLayout &dataLayout) {
  if (isa<FunctionType>(type))
    return dataLayout.getPointerSize();
  // Bug fix: the previous implementation drilled down through nested ArrayTypes
  // to the innermost element type and returned its size, ignoring all array
  // dimensions. For example, [4 x [3 x i32]] would return sizeof(i32)=4 instead
  // of the correct 48 bytes. This caused TypeLayout sizes to be wrong for
  // nested arrays, making offsetMemory() incorrectly return Universal for valid
  // in-bounds accesses.
  //
  // Zero-element arrays ([0 x T]) have an alloc size of 0 from DataLayout,
  // which is correct for our purposes (they model flexible array members; the
  // memory model treats them as byte arrays anyway via the opaque-type path
  // above).
  return dataLayout.getTypeAllocSize(type);
}

void TypeMapBuilder::buildTypeMap() {
  // Step 1: Collect types
  auto typeSet = TypeCollector().runOnModule(module);

  // Step 2: Analyze struct compatibility via casts
  auto structCastMap = StructCastAnalysis().runOnModule(module);

  // Step 3: Build array layouts
  auto arrayLayoutMap = ArrayLayoutAnalysis().runOnTypes(typeSet);

  // Step 4: Build pointer layouts (using cast map for safety)
  auto ptrLayoutMap = PointerLayoutAnalysis(structCastMap).runOnTypes(typeSet);

  // Step 5: Final assembly
  for (auto *type : typeSet) {
    // Some LLVM IR types are unsized (e.g., function types, opaque structs,
    // etc.). We conservatively treat unknown/unsized types as a byte array to
    // avoid triggering DataLayout assertions.
    if (!isa<FunctionType>(type) && !type->isSized()) {
      insertOpaqueType(type);
      continue;
    }

    if (auto *stType = dyn_cast<StructType>(type)) {
      if (stType->isOpaque()) {
        insertOpaqueType(type);
        continue;
      }
    }

    auto typeSize = getTypeSize(type, typeSet.getDataLayout());

    const auto *ptrLayout = ptrLayoutMap.lookup(type);
    assert(ptrLayout != nullptr);

    const auto *arrayLayout = arrayLayoutMap.lookup(type);
    assert(arrayLayout != nullptr);

    insertTypeMap(type, typeSize, arrayLayout, ptrLayout);
  }
}

} // namespace

TypeMap TypeAnalysis::runOnModule(const Module &module) {
  TypeMap typeMap;

  TypeMapBuilder(module, typeMap).buildTypeMap();

  return typeMap;
}

} // namespace tpa

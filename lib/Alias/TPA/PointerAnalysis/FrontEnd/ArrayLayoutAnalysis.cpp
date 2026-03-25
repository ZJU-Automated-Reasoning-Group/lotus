// Implementation of ArrayLayoutAnalysis.
//
// Builds the ArrayLayout for all types in the program.
//
// Key Concepts:
// - Nested Arrays: Recursively flattens nested structures and arrays to
// identify
//   linear memory regions that behave as arrays.
// - Layout Construction:
//   - For Structs: Recursively processes fields, shifting offsets.
//   - For Arrays: Creates a single region {0, total_size, elem_size} +
//   recursive sub-layouts.
// - Opaque Types: Treated conservatively as byte arrays.
//
// This analysis ensures that the MemoryModel can correctly collapse accesses to
// different array indices into a single summary element.

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/ArrayLayoutAnalysis.h"

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeSet.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/ArrayLayout.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

using namespace llvm;

namespace tpa {

namespace {

class ArrayLayoutMapBuilder {
private:
  const TypeSet &typeSet;
  ArrayLayoutMap &arrayLayoutMap;

  void insertMap(const Type *, const ArrayLayout *);

  const ArrayLayout *processStructType(StructType *);
  const ArrayLayout *processArrayType(ArrayType *);
  const ArrayLayout *processSimpleType(Type *);
  const ArrayLayout *processType(Type *);

public:
  ArrayLayoutMapBuilder(const TypeSet &t, ArrayLayoutMap &a)
      : typeSet(t), arrayLayoutMap(a) {}

  void buildArrayLayoutMap();
};

void ArrayLayoutMapBuilder::insertMap(const Type *type,
                                      const ArrayLayout *layout) {
  arrayLayoutMap.insert(type, layout);
}

const ArrayLayout *
ArrayLayoutMapBuilder::processStructType(StructType *stType) {
  // We know nothing about opaque type. Conservatively treat it as a byte array
  if (stType->isOpaque()) {
    const auto *layout = ArrayLayout::getByteArrayLayout();
    insertMap(stType, layout);
    return layout;
  }

  ArrayLayout::ArrayTripleList arrayTripleList;

  const auto *structLayout = typeSet.getDataLayout().getStructLayout(stType);
  for (unsigned i = 0, e = stType->getNumElements(); i != e; ++i) {
    auto offset = structLayout->getElementOffset(i);

    auto *subType = stType->getElementType(i);
    const auto *subLayout = processType(subType);
    if (!subLayout->empty()) {
      // Adjust the start and end field in the sub-layout according to the
      // offset (shift the sub-layout to its position in the struct)
      std::transform(subLayout->begin(), subLayout->end(),
                     std::back_inserter(arrayTripleList),
                     [offset](auto const &triple) -> ArrayTriple {
                       return {triple.start + offset, triple.end + offset,
                               triple.size};
                     });
    }
  }

  const auto *stArrayLayout =
      ArrayLayout::getLayout(std::move(arrayTripleList));
  insertMap(stType, stArrayLayout);
  return stArrayLayout;
}

const ArrayLayout *
ArrayLayoutMapBuilder::processArrayType(ArrayType *arrayType) {
  auto *elemType = arrayType->getElementType();
  auto numElems = arrayType->getNumElements();
  // Array with 0 element is nasty. Treat it as an array with 1 element
  if (numElems == 0)
    numElems = 1;
  auto elemSize = typeSet.getDataLayout().getTypeAllocSize(elemType);

  auto arraySize = numElems * elemSize;
  // Create the main triple for this array: [0, total_size, elem_size]
  ArrayLayout::ArrayTripleList arrayTripleList = {{0, arraySize, elemSize}};

  // We need to examine the element type here because it may contain additional
  // array triple (e.g., array of structs containing arrays)
  const auto *subLayout = processType(elemType);
  if (!subLayout->empty())
    arrayTripleList.insert(arrayTripleList.end(), subLayout->begin(),
                           subLayout->end());

  // Remove duplicate entries in triple list
  // TODO: arrays with size 1 are not "real" arrays. Remove them
  arrayTripleList.erase(
      std::unique(arrayTripleList.begin(), arrayTripleList.end()),
      arrayTripleList.end());

  const auto *arrayLayout = ArrayLayout::getLayout(std::move(arrayTripleList));
  insertMap(arrayType, arrayLayout);
  return arrayLayout;
}

const ArrayLayout *ArrayLayoutMapBuilder::processSimpleType(Type *simpleType) {
  const auto *layout = ArrayLayout::getDefaultLayout();
  insertMap(simpleType, layout);
  return layout;
}

const ArrayLayout *ArrayLayoutMapBuilder::processType(Type *type) {
  const auto *layout = arrayLayoutMap.lookup(type);
  if (layout != nullptr)
    return layout;

  if (auto *stType = dyn_cast<StructType>(type))
    return processStructType(stType);
  else if (auto *arrayType = dyn_cast<ArrayType>(type))
    return processArrayType(arrayType);
  else
    return processSimpleType(type);
}

void ArrayLayoutMapBuilder::buildArrayLayoutMap() {
  for (auto *type : typeSet)
    processType(type);
}

} // namespace

ArrayLayoutMap ArrayLayoutAnalysis::runOnTypes(const TypeSet &typeSet) {
  ArrayLayoutMap arrayLayoutMap;

  ArrayLayoutMapBuilder(typeSet, arrayLayoutMap).buildArrayLayoutMap();

  return arrayLayoutMap;
}

} // namespace tpa

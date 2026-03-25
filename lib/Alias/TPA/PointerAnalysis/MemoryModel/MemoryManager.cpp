// Implementation of the MemoryManager.
//
// The MemoryManager is the central authority for managing the abstract memory
// model. It handles the creation of MemoryObjects and their organization into
// MemoryBlocks.
//
// Concepts:
// - MemoryBlock: Represents a conceptual allocation unit (e.g., a malloc call
// site,
//   a stack variable declaration, or a global variable).
// - MemoryObject: Represents a specific offset within a MemoryBlock (e.g.,
// field 'x'
//   at offset 0 inside struct 'Point').
// - TypeLayout: Used to understand the structure (fields, arrays) of allocated
// memory.
//
// Key Features:
// - Region-based Allocation: Distinct handling for Stack, Heap, Global, and
// Function memory.
// - Field Sensitivity: `offsetMemory` creates new MemoryObjects for field
// accesses.
// - Array Abstraction: Detects array accesses via `TypeLayout` and collapses
// them
//   into summary objects (field-insensitive for array elements, but sensitive
//   for the array itself).

#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"

#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

using namespace context;

namespace tpa {

// Helper to check if the type layout implies that the object at offset 0
// should be treated as a summary (e.g., an array at the start of a struct).
static bool startWithSummary(const TypeLayout *type) {
  bool ret;
  std::tie(std::ignore, ret) = type->offsetInto(0);
  return ret;
}

MemoryManager::MemoryManager(size_t pSize)
    : ptrSize(pSize), argvObj(nullptr), envpObj(nullptr) {}

// Internal factory for MemoryObjects.
// Interns the object in `objSet` to ensure pointer uniqueness.
// Fix #8: Also maintains the blockToObjects index so that
// getReachableMemoryObjects() can look up objects by block in O(k) time
// instead of scanning all objects in O(n).
const MemoryObject *MemoryManager::getMemoryObject(const MemoryBlock *memBlock,
                                                   size_t offset,
                                                   bool summary) const {
  assert(memBlock != nullptr);

  auto obj = MemoryObject(memBlock, offset, summary);
  // C++14-compatible: use std::pair instead of structured binding.
  auto insertResult = objSet.insert(obj);
  auto itr = insertResult.first;
  bool inserted = insertResult.second;
  assert(itr->isSummaryObject() == summary);
  const MemoryObject *result = &*itr;

  // Populate the block→objects index for newly interned objects.
  if (inserted)
    blockToObjects[memBlock].push_back(result);

  return result;
}

// Internal factory for MemoryBlocks.
// Maps an AllocSite (instruction + context) to a MemoryBlock.
const MemoryBlock *MemoryManager::allocateMemoryBlock(AllocSite allocSite,
                                                      const TypeLayout *type) {
  auto itr = allocMap.find(allocSite);
  if (itr == allocMap.end())
    itr = allocMap.insert(
        itr, std::make_pair(allocSite, MemoryBlock(allocSite, type)));
  assert(type == itr->second.getTypeLayout());
  return &itr->second;
}

// Allocates memory for a global variable.
// Globals are context-insensitive (Global AllocSite).
const MemoryObject *
MemoryManager::allocateGlobalMemory(const llvm::GlobalVariable *value,
                                    const TypeLayout *type) {
  assert(value != nullptr && type != nullptr);

  const auto *memBlock =
      allocateMemoryBlock(AllocSite::getGlobalAllocSite(value), type);
  return getMemoryObject(memBlock, 0, startWithSummary(type));
}

// Allocates memory for a function (to support function pointers).
// Treats the function as a memory block with 0 size.
const MemoryObject *
MemoryManager::allocateMemoryForFunction(const llvm::Function *f) {
  const auto *memBlock =
      allocateMemoryBlock(AllocSite::getFunctionAllocSite(f),
                          TypeLayout::getPointerTypeLayoutWithSize(0));
  return getMemoryObject(memBlock, 0, false);
}

// Allocates stack memory (alloca).
// Context-sensitive based on the current context.
const MemoryObject *MemoryManager::allocateStackMemory(const Context *ctx,
                                                       const llvm::Value *ptr,
                                                       const TypeLayout *type) {
  const auto *memBlock =
      allocateMemoryBlock(AllocSite::getStackAllocSite(ctx, ptr), type);
  return getMemoryObject(memBlock, 0, startWithSummary(type));
}

// Allocates heap memory (malloc/new).
// Context-sensitive. Heap objects are ALWAYS summary objects (weak updates)
// because multiple runtime allocations map to the same abstract object.
const MemoryObject *MemoryManager::allocateHeapMemory(const Context *ctx,
                                                      const llvm::Value *ptr,
                                                      const TypeLayout *type) {
  const auto *memBlock =
      allocateMemoryBlock(AllocSite::getHeapAllocSite(ctx, ptr), type);
  // Note: summary=true passed to getMemoryObject
  return getMemoryObject(memBlock, 0, true);
}

// Special allocation for argv (array of strings).
const MemoryObject *MemoryManager::allocateArgv(const llvm::Value *ptr) {
  const auto *memBlock = allocateMemoryBlock(
      AllocSite::getStackAllocSite(Context::getGlobalContext(), ptr),
      TypeLayout::getByteArrayTypeLayout());
  argvObj = getMemoryObject(memBlock, 0, true);
  return argvObj;
}

// Special allocation for envp (environment pointer).
const MemoryObject *MemoryManager::allocateEnvp(const llvm::Value *ptr) {
  const auto *memBlock = allocateMemoryBlock(
      AllocSite::getStackAllocSite(Context::getGlobalContext(), ptr),
      TypeLayout::getByteArrayTypeLayout());
  envpObj = getMemoryObject(memBlock, 0, true);
  return envpObj;
}

// Calculates a new MemoryObject given a base object and an offset.
// This is the core of field sensitivity.
const MemoryObject *MemoryManager::offsetMemory(const MemoryObject *obj,
                                                size_t offset) const {
  assert(obj != nullptr);

  if (offset == 0)
    return obj;
  else
    return offsetMemory(obj->getMemoryBlock(), obj->getOffset() + offset);
}

// Low-level offset calculation.
// Determines the new offset and whether it falls into an array (requiring
// summary).
const MemoryObject *MemoryManager::offsetMemory(const MemoryBlock *block,
                                                size_t offset) const {
  assert(block != nullptr);

  // Universal/Null blocks are invariant to offset.
  if (block == &uBlock || block == &nBlock)
    return &uObj;

  const auto *type = block->getTypeLayout();

  bool summary;
  size_t adjustedOffset;
  // Consult TypeLayout to see if 'offset' hits an array region.
  // adjustedOffset will be normalized (modulo element size) if inside an array.
  std::tie(adjustedOffset, summary) = type->offsetInto(offset);

  // Heap objects are inherently summary objects.
  summary = summary || block->isHeapBlock();

  // Bounds check
  if (adjustedOffset >= type->getSize())
    return &uObj;

  return getMemoryObject(block, adjustedOffset, summary);
}

// Returns all pointer-typed fields reachable from 'obj'.
// Used for copying (e.g., struct assignment) to ensure deep copies of pointers.
std::vector<const MemoryObject *>
MemoryManager::getReachablePointerObjects(const MemoryObject *obj,
                                          bool includeSelf) const {
  auto ret = std::vector<const MemoryObject *>();
  if (includeSelf)
    ret.push_back(obj);

  if (!obj->isSpecialObject()) {
    const auto *memBlock = obj->getMemoryBlock();
    const auto *ptrLayout = memBlock->getTypeLayout()->getPointerLayout();

    // Find all valid pointer offsets in the type layout starting from
    // obj->getOffset()
    auto itr = ptrLayout->lower_bound(obj->getOffset());
    // (Optimization: skip self if found, as it was handled by includeSelf)
    if (itr != ptrLayout->end() && *itr == obj->getOffset())
      ++itr;

    // Transform offsets back into MemoryObjects
    std::transform(itr, ptrLayout->end(), std::back_inserter(ret),
                   [this, memBlock](size_t offset) {
                     return offsetMemory(memBlock, offset);
                   });
  }

  return ret;
}

// Returns all abstract memory objects that belong to the same MemoryBlock
// as 'obj' and are currently instantiated in the manager.
//
// Fix #8: Use the blockToObjects index (populated in getMemoryObject) to
// look up objects by block in O(k) time instead of scanning all objects in
// O(n). This is a significant performance improvement for large programs
// where the total number of memory objects can be in the millions.
std::vector<const MemoryObject *>
MemoryManager::getReachableMemoryObjects(const MemoryObject *obj) const {
  if (obj->isSpecialObject())
    return {obj};

  const auto *block = obj->getMemoryBlock();
  auto itr = blockToObjects.find(block);
  if (itr == blockToObjects.end())
    return {};
  return itr->second;
}

} // namespace tpa
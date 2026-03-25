#pragma once

#include "Alias/TPA/PointerAnalysis/MemoryModel/AllocSite.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryBlock.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"

#include <set>
#include <unordered_map>
#include <vector>

namespace tpa {

// Memory manager for pointer analysis
//
// Manages memory objects and their allocation sites. Memory objects are the
// abstract representation of memory locations that pointers can point to.
//
// Memory Model:
// - Field-sensitive: Each field of a struct is a separate memory object
// - Allocation site based: Objects are identified by their allocation site
// - Context-sensitive: Stack/heap allocations include context
//
// Special Objects:
// - Universal object: Represents "may point to any memory"
// - Null object: Represents the null pointer
// - argv/envp objects: Special handling for command line arguments
//
// Allocation Types:
// - Global: Global variables
// - Function: Function objects (for function pointers)
// - Stack: Local variables (includes context)
// - Heap: malloc/alloca allocations (includes context)
class MemoryManager {
private:
  // Maps allocation sites to memory blocks
  // Memory blocks contain type layout information
  using AllocMap = std::unordered_map<AllocSite, MemoryBlock>;
  AllocMap allocMap;

  // Size of a pointer in bytes (typically 8 on 64-bit systems)
  size_t ptrSize;

  // Ordered set of all memory objects
  // Ordered for deterministic iteration and comparison
  mutable std::set<MemoryObject> objSet;

  // Fix #8: Index from MemoryBlock* to the list of MemoryObjects that belong
  // to it. This allows getReachableMemoryObjects() to run in O(k) time (where
  // k is the number of fields/elements in the block) instead of O(n) over all
  // objects in the analysis. The index is populated lazily in getMemoryObject()
  // whenever a new MemoryObject is interned into objSet.
  mutable std::unordered_map<const MemoryBlock *,
                             std::vector<const MemoryObject *>>
      blockToObjects;

  // Universal memory block: represents memory that may contain any value
  // Points to this object means "may point to anything"
  static const MemoryBlock uBlock;
  // Null memory block: represents the null pointer
  // Points to this object means "is definitely null"
  static const MemoryBlock nBlock;
  static const MemoryObject uObj;
  static const MemoryObject nObj;

  // Special objects for command line arguments
  const MemoryObject *argvObj;
  const MemoryObject *envpObj;

  // Create a memory block for an allocation site
  const MemoryBlock *allocateMemoryBlock(AllocSite, const TypeLayout *);
  // Get or create a memory object at a specific offset
  const MemoryObject *getMemoryObject(const MemoryBlock *, size_t, bool) const;

  // Get memory object at offset within a block
  const MemoryObject *offsetMemory(const MemoryBlock *, size_t) const;

public:
  // Constructor
  // Parameters: pSize - size of a pointer in bytes (default 8)
  MemoryManager(size_t pSize = 8u);

  // Get special objects
  static const MemoryObject *getUniversalObject() { return &uObj; }
  static const MemoryObject *getNullObject() { return &nObj; }
  // Get pointer size
  size_t getPointerSize() const { return ptrSize; }

  // Create memory object for a global variable
  const MemoryObject *allocateGlobalMemory(const llvm::GlobalVariable *,
                                           const TypeLayout *);
  // Create memory object for a function (for function pointers)
  const MemoryObject *allocateMemoryForFunction(const llvm::Function *f);
  // Create memory object for a stack allocation (local variable)
  const MemoryObject *allocateStackMemory(const context::Context *,
                                          const llvm::Value *,
                                          const TypeLayout *);
  // Create memory object for a heap allocation (malloc/new)
  const MemoryObject *allocateHeapMemory(const context::Context *,
                                         const llvm::Value *,
                                         const TypeLayout *);

  // Create memory object for argv (command line arguments array)
  const MemoryObject *allocateArgv(const llvm::Value *);
  // Create memory object for envp (environment variables array)
  const MemoryObject *allocateEnvp(const llvm::Value *);
  // Access special objects
  const MemoryObject *getArgvObject() const {
    assert(argvObj != nullptr);
    return argvObj;
  }
  const MemoryObject *getEnvpObject() const {
    assert(envpObj != nullptr);
    return envpObj;
  }

  // Get memory object at offset within another object
  const MemoryObject *offsetMemory(const MemoryObject *, size_t) const;
  // Get all memory objects that share the same memory block
  // This includes all fields of a struct or elements of an array
  std::vector<const MemoryObject *>
  getReachableMemoryObjects(const MemoryObject *) const;
  // Get all memory objects that might contain pointers
  // Filters to only objects of pointer type within the same block
  std::vector<const MemoryObject *>
  getReachablePointerObjects(const MemoryObject *,
                             bool includeSelf = true) const;
};

} // namespace tpa
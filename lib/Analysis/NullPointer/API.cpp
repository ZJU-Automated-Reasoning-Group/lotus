#include "Analysis/NullPointer/API.h"

#include <set>
#include <string>

#include <llvm/IR/Instructions.h>

// Set of function names that perform heap allocation.
std::set<std::string> API::HeapAllocFunctions = {
    "malloc",
    "calloc",
    "memalign",
    "aligned_alloc",
    "pvalloc",
    "valloc",
    "strdup",
    "strndup",
    "kmalloc",
    "mmap",
    "mmap64",
    "get_current_dir_name",
    "_Znwj",               // operator new(unsigned int)
    "_Znwm",               // operator new(unsigned long)
    "_Znaj",               // operator new[](unsigned int)
    "_Znam",               // operator new[](unsigned long)
    "_ZnwjRKSt9nothrow_t", // operator new(unsigned int)
    "_ZnwmRKSt9nothrow_t", // operator new(unsigned long)
    "_ZnajRKSt9nothrow_t", // operator new[](unsigned int)
    "_ZnamRKSt9nothrow_t", // operator new[](unsigned long)
    "realloc",
    "reallocf",
    "getline",
    "getwline",
    "getdelim",
    "getwdelim",
};

// Returns true if the instruction performs memory allocation (heap or stack).
bool API::isMemoryAllocate(Instruction *I) {
  return isHeapAllocate(I) || isStackAllocate(I);
}

// Returns true if the instruction is a call to a heap allocation function.
bool API::isHeapAllocate(Instruction *I) {
  if (auto *CI = dyn_cast<CallInst>(I)) {
    if (auto *Callee = CI->getCalledFunction()) {
      return HeapAllocFunctions.count(Callee->getName().str());
    }
  }
  return false;
}

// Returns true if the instruction is a stack allocation (alloca).
bool API::isStackAllocate(Instruction *I) { return isa<AllocaInst>(I); }

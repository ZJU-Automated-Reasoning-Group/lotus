#ifndef SUPPORT_API_H
#define SUPPORT_API_H

#include <set>

#include <llvm/IR/Instruction.h>

using namespace llvm;

class API {
public:
  static bool isMemoryAllocate(Instruction *);

  static bool isHeapAllocate(Instruction *);

  static bool isStackAllocate(Instruction *);

  static std::set<std::string> HeapAllocFunctions;
};

#endif // SUPPORT_API_H

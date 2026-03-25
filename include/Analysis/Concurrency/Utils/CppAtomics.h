/**
 * @file CppAtomics.h
 * @brief C++ Atomics Recognition (C++11/14/17/20)
 *
 * This file provides helpers to identify C++ atomic operations
 * and their memory ordering in LLVM IR. The C++ memory model
 * and atomic operations are consistent across C++11, C++14, C++17, and C++20.
 *
 * @author rainoftime
 * @date 2025-2026
 */

#ifndef CPP_ATOMICS_H
#define CPP_ATOMICS_H

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace CppAtomics {

// Enum to represent C++11 memory orderings
enum class MemoryOrder {
  NotAtomic,
  Relaxed,
  Consume,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent
};

// Functions to identify atomic operations
bool isAtomic(const llvm::Instruction *inst);
MemoryOrder getMemoryOrder(const llvm::Instruction *inst);
const llvm::Value *getAtomicPointer(const llvm::Instruction *inst);

// Functions to check for specific properties
bool isLockFree(const llvm::Instruction *inst);
bool isStore(const llvm::Instruction *inst);
bool isLoad(const llvm::Instruction *inst);
bool isReadModifyWrite(const llvm::Instruction *inst);
bool isCompareExchange(const llvm::Instruction *inst);
bool isFence(const llvm::Instruction *inst);

// Memory ordering property checks for synchronization analysis
bool hasAcquireSemantics(const llvm::Instruction *inst);
bool hasReleaseSemantics(const llvm::Instruction *inst);
bool hasSequentialConsistency(const llvm::Instruction *inst);
bool isRelaxed(const llvm::Instruction *inst);

// Synchronizes-with relationship helpers
// Direct atomic synchronization is exact-location only here; callers that want
// alias-aware matching should perform that separately before relying on the
// result. Fence-based synchronization remains witness-driven.
bool canSynchronizeWith(const llvm::Instruction *release, const llvm::Instruction *acquire);
bool participatesInReleaseSequence(const llvm::Instruction *inst);

// Fence analysis
bool isFenceAcquire(const llvm::Instruction *inst);
bool isFenceRelease(const llvm::Instruction *inst);
bool isFenceAcqRel(const llvm::Instruction *inst);
bool isFenceSeqCst(const llvm::Instruction *inst);

// Helper to get human-readable memory order string
const char* memoryOrderToString(MemoryOrder order);

} // namespace CppAtomics

#endif // CPP_ATOMICS_H

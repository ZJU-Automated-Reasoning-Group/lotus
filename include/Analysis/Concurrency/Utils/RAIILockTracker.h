/**
 * @file RAIILockTracker.h
 * @brief RAII Lock Lifetime Tracking
 *
 * This file provides utilities for tracking RAII lock objects (lock_guard,
 * unique_lock, scoped_lock, shared_lock) and mapping their constructors to
 * destructors to synthesize acquire/release operations.
 *
 * @author rainoftime
 * @date 2026
 */

#ifndef RAII_LOCK_TRACKER_H
#define RAII_LOCK_TRACKER_H

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace RAIILock {

enum class OwnershipKind {
  Immediate,
  Deferred,
  Try,
  Adopt,
  Unknown
};

/**
 * @struct LockLifetime
 * @brief Represents the lifetime of a RAII lock object
 *
 * Tracks the constructor call (acquire), destructor calls (release),
 * and the lock object allocation site.
 */
struct LockLifetime {
  const llvm::Value *lockObject;          ///< Canonical identity of the lock object
  const llvm::CallBase *constructor;      ///< Constructor call (acquire point)
  std::vector<const llvm::Instruction *> destructors; ///< Destructor calls (release points)
  std::vector<const llvm::Value *> underlyingLocks;   ///< Ordered mutexes protected by this RAII object
  std::vector<bool> sharedModes;                      ///< Per-lock mode: true for shared/read, false for exclusive
  OwnershipKind ownership = OwnershipKind::Immediate; ///< Constructor ownership policy
  bool isScoped;                          ///< True for scoped_lock (multi-lock)
  bool hasPreciseLifetimeEnd = false;    ///< Explicit destructor or lifetime.end was found
  const llvm::Instruction *impreciseLifetimeBoundary =
      nullptr; ///< First instruction after the last visible lock-object use
};

/**
 * @class RAIILockTracker
 * @brief Tracks RAII lock object lifetimes in a function
 *
 * Analyzes a function to identify RAII lock objects and map their
 * constructor/destructor pairs for synchronization analysis.
 */
class RAIILockTracker {
private:
  /// Map from lock object allocation to its lifetime info
  std::map<const llvm::Value *, LockLifetime> lockLifetimes;
  
  /// Set of destructor calls we've already processed
  std::set<const llvm::Instruction *> processedDestructors;

public:
  RAIILockTracker() = default;

  /**
   * @brief Analyze a function to track all RAII lock objects
   * @param F The function to analyze
   */
  void analyzeFunction(const llvm::Function *F);

  /**
   * @brief Get the lifetime information for a lock object
   * @param alloca The allocation site of the lock object
   * @return Pointer to LockLifetime if found, nullptr otherwise
   */
  const LockLifetime *getLockLifetime(const llvm::Value *lockObject) const;

  /**
   * @brief Check if an instruction is a RAII lock constructor
   * @param inst The instruction to check
   * @return True if this is a RAII lock constructor call
   */
  static bool isRAIILockConstructor(const llvm::Instruction *inst);

  /**
   * @brief Check if an instruction is a RAII lock destructor
   * @param inst The instruction to check
   * @return True if this is a RAII lock destructor call
   */
  static bool isRAIILockDestructor(const llvm::Instruction *inst);

  /**
   * @brief Get all lock lifetimes tracked in this function
   * @return Map of lock-object identity -> LockLifetime
   */
  const std::map<const llvm::Value *, LockLifetime> &getAllLockLifetimes() const {
    return lockLifetimes;
  }

  /**
   * @brief Find the lock object allocation for a constructor call
   * @param ctor Constructor call instruction
   * @return The canonical lock-object identity, or nullptr
   */
  static const llvm::Value *findLockObjectForConstructor(const llvm::CallBase *ctor);

  /**
   * @brief Extract the underlying mutex/lock from a constructor call
   * @param ctor Constructor call instruction
   * @return The mutex value being locked, or nullptr
   */
  static std::vector<const llvm::Value *>
  extractUnderlyingLocks(const llvm::CallBase *ctor);

  /**
   * @brief Check if this is a shared (read) lock
   * @param inst The instruction to check
   * @return True if this acquires a shared/read lock
   */
  static bool isSharedLock(const llvm::Instruction *inst);

  /// Determine the constructor ownership policy for a wrapper object.
  static OwnershipKind
  getOwnershipKind(const llvm::CallBase *ctor);

  /**
   * @brief Find all destructor calls for a lock object
   * @param lockObject The canonical lock-object identity
   * @param F The function containing the lock object
   * @return Vector of destructor call sites
   */
  static std::vector<const llvm::Instruction *> findDestructorsForLockObject(
      const llvm::Value *lockObject, const llvm::Function *F);

private:
  /**
   * @brief Process a single lock constructor
   * @param ctor The constructor call
   * @param F The containing function
   */
  void processConstructor(const llvm::CallBase *ctor, const llvm::Function *F);
};

} // namespace RAIILock

#endif // RAII_LOCK_TRACKER_H

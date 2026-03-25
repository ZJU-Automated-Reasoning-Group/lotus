/**
 * @file CppThreading.h
 * @brief C++ Threading Primitives Language Model (C++11/17/20)
 *
 * This file provides pattern matching and recognition for C++ standard library
 * threading primitives including std::thread, std::mutex, std::condition_variable,
 * std::shared_mutex (C++17), std::jthread, std::latch, std::barrier, std::semaphore (C++20),
 * and RAII lock wrappers.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>

namespace CppThreadingModel {

inline bool isStdThreadEntity(const llvm::StringRef &funcName) {
  return funcName.contains("St6thread") || funcName.contains("St3__16thread");
}

inline bool containsCtorCode(const llvm::StringRef &funcName) {
  return funcName.contains("C1E") || funcName.contains("C2E") ||
         funcName.contains("C1I") || funcName.contains("C2I");
}

inline bool containsDtorCode(const llvm::StringRef &funcName) {
  return funcName.contains("D1Ev") || funcName.contains("D2Ev");
}

inline bool isTaggedWrapperCtor(const llvm::StringRef &funcName,
                                const llvm::StringRef &wrapper,
                                const llvm::StringRef &tag) {
  return funcName.contains(wrapper) && containsCtorCode(funcName) &&
         funcName.contains(tag);
}

// Check if the function is std::thread constructor (fork)
inline bool isFork(const llvm::StringRef& funcName) {
  // Thread-launch constructors are templated and carry callable argument encodings.
  // Exclude move/copy/default ctors to avoid classifying non-launch operations.
  if (!isStdThreadEntity(funcName) || !containsCtorCode(funcName)) {
    return false;
  }
  if (funcName.contains("EOS_") || funcName.contains("ERKS_") ||
      funcName.contains("C1Ev") || funcName.contains("C2Ev")) {
    return false;
  }
  return funcName.contains("threadC") || funcName.contains("threadIC");
}

// Check if the function is std::thread::join
inline bool isJoin(const llvm::StringRef& funcName) {
  return isStdThreadEntity(funcName) && funcName.contains("thread4joinEv");
}

// Check if the function is std::thread::detach
inline bool isDetach(const llvm::StringRef& funcName) {
  return isStdThreadEntity(funcName) && funcName.contains("thread6detachEv");
}

// Check if the function is std::mutex::lock or std::recursive_mutex::lock.
// Deliberately excludes shared_mutex (handled separately by isSharedLock*).
inline bool isAcquire(const llvm::StringRef& funcName) {
  // std::mutex::lock -> _ZNSt5mutex4lockEv
  // std::recursive_mutex::lock -> _ZNSt15recursive_mutex4lockEv
  // Exclude shared_mutex variants so they are not double-counted.
  return funcName.contains("mutex") && funcName.contains("lockEv") &&
         !funcName.contains("unlock") && !funcName.contains("shared");
}

// Check if the function is std::mutex::try_lock (excludes shared_mutex).
inline bool isTryAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("mutex") && funcName.contains("try_lockEv") &&
         !funcName.contains("shared");
}

// Check if the function is std::mutex::unlock (excludes shared_mutex).
inline bool isRelease(const llvm::StringRef& funcName) {
  return funcName.contains("mutex") && funcName.contains("unlockEv") &&
         !funcName.contains("shared");
}

// Check if the function is std::condition_variable::wait
inline bool isCondWait(const llvm::StringRef& funcName) {
  return funcName.contains("condition_variable") && funcName.contains("wait");
}

// Check if the function is std::condition_variable::notify_one
inline bool isCondSignal(const llvm::StringRef& funcName) {
  return funcName.contains("condition_variable") && funcName.contains("notify_one");
}

// Check if the function is std::condition_variable::notify_all
inline bool isCondBroadcast(const llvm::StringRef& funcName) {
  return funcName.contains("condition_variable") && funcName.contains("notify_all");
}

// C++17 std::shared_mutex support
// Check for std::shared_mutex::lock_shared (read lock)
inline bool isSharedLockAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("shared_mutex") &&
         funcName.contains("lock_sharedEv") &&
         !funcName.contains("unlock");
}

// Check for std::shared_mutex::lock (write/exclusive lock)
inline bool isSharedLockExclusiveAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("shared_mutex") && funcName.contains("lockEv") && !funcName.contains("unlock");
}

// Check for std::shared_mutex::unlock_shared
inline bool isSharedLockRelease(const llvm::StringRef& funcName) {
  return funcName.contains("shared_mutex") && funcName.contains("unlock_sharedEv");
}

// Check for std::shared_mutex::unlock
inline bool isSharedLockExclusiveRelease(const llvm::StringRef& funcName) {
  return funcName.contains("shared_mutex") && funcName.contains("unlockEv") && !funcName.contains("unlock_shared");
}

// Check for std::shared_timed_mutex operations
inline bool isSharedTimedLockAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("shared_timed_mutex") &&
         funcName.contains("lock_sharedEv") &&
         !funcName.contains("unlock");
}

inline bool isSharedTimedLockExclusiveAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("shared_timed_mutex") && funcName.contains("lockEv") && !funcName.contains("unlock");
}

inline bool isSharedTimedLockRelease(const llvm::StringRef& funcName) {
  return funcName.contains("shared_timed_mutex") &&
         funcName.contains("unlock_sharedEv");
}

inline bool isSharedTimedLockExclusiveRelease(const llvm::StringRef& funcName) {
  return funcName.contains("shared_timed_mutex") &&
         funcName.contains("unlockEv") &&
         !funcName.contains("unlock_shared");
}

// RAII lock wrappers - constructors act as acquire
inline bool isLockGuardConstructor(const llvm::StringRef& funcName) {
  return funcName.contains("lock_guard") && containsCtorCode(funcName);
}

inline bool isUniqueLockConstructor(const llvm::StringRef& funcName) {
  return funcName.contains("unique_lock") && containsCtorCode(funcName);
}

inline bool isScopedLockConstructor(const llvm::StringRef& funcName) {
  return funcName.contains("scoped_lock") && containsCtorCode(funcName);
}

inline bool isSharedLockConstructor(const llvm::StringRef& funcName) {
  return funcName.contains("shared_lock") && containsCtorCode(funcName);
}

// RAII lock wrappers - destructors act as release
inline bool isLockGuardDestructor(const llvm::StringRef& funcName) {
  return funcName.contains("lock_guard") && containsDtorCode(funcName);
}

inline bool isUniqueLockDestructor(const llvm::StringRef& funcName) {
  return funcName.contains("unique_lock") && containsDtorCode(funcName);
}

inline bool isScopedLockDestructor(const llvm::StringRef& funcName) {
  return funcName.contains("scoped_lock") && containsDtorCode(funcName);
}

inline bool isSharedLockDestructor(const llvm::StringRef& funcName) {
  return funcName.contains("shared_lock") && containsDtorCode(funcName);
}

// std::unique_lock manual lock/unlock
inline bool isUniqueLockLock(const llvm::StringRef& funcName) {
  return funcName.contains("unique_lock") && funcName.contains("lockEv") &&
         !funcName.contains("unlock");
}

inline bool isUniqueLockUnlock(const llvm::StringRef& funcName) {
  return funcName.contains("unique_lock") && funcName.contains("unlockEv");
}

// C++11 std::call_once
inline bool isCallOnce(const llvm::StringRef& funcName) {
  return funcName.contains("call_once");
}

// C++11 std::future/promise synchronization
inline bool isFutureGet(const llvm::StringRef& funcName) {
  return funcName.contains("future") && (funcName.contains("getEv") || funcName.contains("3getEv"));
}

inline bool isFutureWait(const llvm::StringRef& funcName) {
  return funcName.contains("future") && funcName.contains("waitEv");
}

inline bool isPromiseSetValue(const llvm::StringRef& funcName) {
  return funcName.contains("promise") && funcName.contains("set_value");
}

inline bool isPromiseSetException(const llvm::StringRef& funcName) {
  return funcName.contains("promise") && funcName.contains("set_exception");
}

// C++11 std::async - task creation
inline bool isAsync(const llvm::StringRef& funcName) {
  return funcName.contains("async") && funcName.contains("_ZNSt");
}

// C++20 std::jthread
inline bool isJthreadConstructor(const llvm::StringRef& funcName) {
  if (!funcName.contains("jthread") || !containsCtorCode(funcName)) {
    return false;
  }
  if (funcName.contains("EOS_") || funcName.contains("ERKS_") ||
      funcName.contains("C1Ev") || funcName.contains("C2Ev")) {
    return false;
  }
  return funcName.contains("jthreadC") || funcName.contains("jthreadIC");
}

inline bool isJthreadJoin(const llvm::StringRef& funcName) {
  return funcName.contains("jthread") && funcName.contains("joinEv");
}

inline bool isJthreadDetach(const llvm::StringRef& funcName) {
  return funcName.contains("jthread") && funcName.contains("detachEv");
}

inline bool isJthreadDestructor(const llvm::StringRef& funcName) {
  return funcName.contains("jthread") && containsDtorCode(funcName);
}

inline bool isAtomicWait(const llvm::StringRef& funcName) {
  return funcName.contains("_ZNSt") && funcName.contains("atomic") &&
         funcName.contains("wait");
}

inline bool isAtomicNotifyOne(const llvm::StringRef& funcName) {
  return funcName.contains("_ZNSt") && funcName.contains("atomic") &&
         funcName.contains("notify_one");
}

inline bool isAtomicNotifyAll(const llvm::StringRef& funcName) {
  return funcName.contains("_ZNSt") && funcName.contains("atomic") &&
         funcName.contains("notify_all");
}

// C++20 std::latch
inline bool isLatchCountDown(const llvm::StringRef& funcName) {
  return funcName.contains("latch") && funcName.contains("count_down");
}

inline bool isLatchWait(const llvm::StringRef& funcName) {
  return funcName.contains("latch") && funcName.contains("waitEv");
}

inline bool isLatchArriveAndWait(const llvm::StringRef& funcName) {
  return funcName.contains("latch") && funcName.contains("arrive_and_wait");
}

// C++20 std::barrier
inline bool isBarrierArriveAndWait(const llvm::StringRef& funcName) {
  return funcName.contains("barrier") && funcName.contains("arrive_and_wait");
}

inline bool isBarrierArrive(const llvm::StringRef& funcName) {
  return funcName.contains("barrier") && funcName.contains("arriveEv") && !funcName.contains("arrive_and_wait");
}

inline bool isBarrierWait(const llvm::StringRef& funcName) {
  return funcName.contains("barrier") && funcName.contains("waitE");
}

// C++20 std::counting_semaphore / std::binary_semaphore
inline bool isSemaphoreAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("semaphore") && funcName.contains("acquireEv");
}

inline bool isSemaphoreRelease(const llvm::StringRef& funcName) {
  return funcName.contains("semaphore") && funcName.contains("releaseE");
}

inline bool isSemaphoreTryAcquire(const llvm::StringRef& funcName) {
  return funcName.contains("semaphore") && funcName.contains("try_acquire");
}

inline bool isDeferLockConstructor(const llvm::StringRef &funcName) {
  return isTaggedWrapperCtor(funcName, "unique_lock", "defer_lock") ||
         isTaggedWrapperCtor(funcName, "shared_lock", "defer_lock");
}

inline bool isTryToLockConstructor(const llvm::StringRef &funcName) {
  return isTaggedWrapperCtor(funcName, "unique_lock", "try_to_lock") ||
         isTaggedWrapperCtor(funcName, "shared_lock", "try_to_lock");
}

inline bool isAdoptLockConstructor(const llvm::StringRef &funcName) {
  return isTaggedWrapperCtor(funcName, "lock_guard", "adopt_lock") ||
         isTaggedWrapperCtor(funcName, "unique_lock", "adopt_lock") ||
         isTaggedWrapperCtor(funcName, "shared_lock", "adopt_lock");
}

} // namespace CppThreadingModel

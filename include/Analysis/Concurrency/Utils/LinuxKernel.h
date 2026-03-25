/**
 * @file LinuxKernel.h
 * @brief Linux Kernel Concurrency Primitives Language Model
 *
 * This file provides pattern matching and recognition for Linux kernel
 * concurrency primitives including spinlocks, mutexes, semaphores, RCU,
 * completion variables, wait queues, and atomic operations.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>

namespace LinuxKernelModel {

// ============================================================================
// Spinlocks
// ============================================================================

inline bool isSpinLockInit(const llvm::StringRef& funcName) {
  return funcName.equals("spin_lock_init") || funcName.equals("raw_spin_lock_init");
}

inline bool isSpinLock(const llvm::StringRef& funcName) {
  return funcName.equals("spin_lock") || funcName.equals("raw_spin_lock") ||
         funcName.equals("spin_lock_irq") || funcName.equals("raw_spin_lock_irq") ||
         funcName.equals("spin_lock_irqsave") || funcName.equals("raw_spin_lock_irqsave") ||
         funcName.equals("spin_lock_bh") || funcName.equals("raw_spin_lock_bh");
}

inline bool isSpinUnlock(const llvm::StringRef& funcName) {
  return funcName.equals("spin_unlock") || funcName.equals("raw_spin_unlock") ||
         funcName.equals("spin_unlock_irq") || funcName.equals("raw_spin_unlock_irq") ||
         funcName.equals("spin_unlock_irqrestore") || funcName.equals("raw_spin_unlock_irqrestore") ||
         funcName.equals("spin_unlock_bh") || funcName.equals("raw_spin_unlock_bh");
}

inline bool isSpinTryLock(const llvm::StringRef& funcName) {
  return funcName.equals("spin_trylock") || funcName.equals("raw_spin_trylock");
}

// ============================================================================
// Mutexes
// ============================================================================

inline bool isMutexInit(const llvm::StringRef& funcName) {
  return funcName.equals("mutex_init") || funcName.equals("__mutex_init");
}

inline bool isMutexLock(const llvm::StringRef& funcName) {
  return funcName.equals("mutex_lock") || funcName.equals("mutex_lock_interruptible") ||
         funcName.equals("mutex_lock_killable") || funcName.equals("mutex_lock_nested");
}

inline bool isMutexUnlock(const llvm::StringRef& funcName) {
  return funcName.equals("mutex_unlock");
}

inline bool isMutexTryLock(const llvm::StringRef& funcName) {
  return funcName.equals("mutex_trylock");
}

inline bool isMutexIsLocked(const llvm::StringRef& funcName) {
  return funcName.equals("mutex_is_locked");
}

// ============================================================================
// Semaphores
// ============================================================================

inline bool isSemaInit(const llvm::StringRef& funcName) {
  return funcName.equals("sema_init") || funcName.equals("init_MUTEX") ||
         funcName.equals("init_MUTEX_LOCKED");
}

inline bool isDown(const llvm::StringRef& funcName) {
  return funcName.equals("down") || funcName.equals("down_interruptible") ||
         funcName.equals("down_killable") || funcName.equals("down_trylock");
}

inline bool isUp(const llvm::StringRef& funcName) {
  return funcName.equals("up");
}

// ============================================================================
// Read-Write Locks
// ============================================================================

inline bool isReadLock(const llvm::StringRef& funcName) {
  return funcName.equals("read_lock") || funcName.equals("read_lock_irq") ||
         funcName.equals("read_lock_irqsave") || funcName.equals("read_lock_bh");
}

inline bool isReadUnlock(const llvm::StringRef& funcName) {
  return funcName.equals("read_unlock") || funcName.equals("read_unlock_irq") ||
         funcName.equals("read_unlock_irqrestore") || funcName.equals("read_unlock_bh");
}

inline bool isWriteLock(const llvm::StringRef& funcName) {
  return funcName.equals("write_lock") || funcName.equals("write_lock_irq") ||
         funcName.equals("write_lock_irqsave") || funcName.equals("write_lock_bh");
}

inline bool isWriteUnlock(const llvm::StringRef& funcName) {
  return funcName.equals("write_unlock") || funcName.equals("write_unlock_irq") ||
         funcName.equals("write_unlock_irqrestore") || funcName.equals("write_unlock_bh");
}

// Read-Write Semaphores
inline bool isDownRead(const llvm::StringRef& funcName) {
  return funcName.equals("down_read") || funcName.equals("down_read_trylock");
}

inline bool isUpRead(const llvm::StringRef& funcName) {
  return funcName.equals("up_read");
}

inline bool isDownWrite(const llvm::StringRef& funcName) {
  return funcName.equals("down_write") || funcName.equals("down_write_trylock");
}

inline bool isUpWrite(const llvm::StringRef& funcName) {
  return funcName.equals("up_write");
}

inline bool isInitRwsem(const llvm::StringRef& funcName) {
  return funcName.equals("init_rwsem");
}

// ============================================================================
// RCU (Read-Copy-Update)
// ============================================================================

inline bool isRcuReadLock(const llvm::StringRef& funcName) {
  return funcName.equals("rcu_read_lock") || funcName.equals("__rcu_read_lock");
}

inline bool isRcuReadUnlock(const llvm::StringRef& funcName) {
  return funcName.equals("rcu_read_unlock") || funcName.equals("__rcu_read_unlock");
}

inline bool isSynchronizeRcu(const llvm::StringRef& funcName) {
  return funcName.equals("synchronize_rcu") || funcName.equals("synchronize_rcu_expedited") ||
         funcName.equals("synchronize_srcu");
}

inline bool isCallRcu(const llvm::StringRef& funcName) {
  return funcName.equals("call_rcu") || funcName.equals("call_srcu");
}

inline bool isRcuDereference(const llvm::StringRef& funcName) {
  return funcName.equals("rcu_dereference") || funcName.equals("rcu_dereference_check") ||
         funcName.equals("rcu_dereference_protected");
}

inline bool isRcuAssignPointer(const llvm::StringRef& funcName) {
  return funcName.equals("rcu_assign_pointer");
}

// ============================================================================
// Seq Locks
// ============================================================================

inline bool isSeqlockInit(const llvm::StringRef& funcName) {
  return funcName.equals("seqlock_init");
}

inline bool isReadSeqbegin(const llvm::StringRef& funcName) {
  return funcName.equals("read_seqbegin") || funcName.equals("read_seqbegin_irqsave");
}

inline bool isReadSeqretry(const llvm::StringRef& funcName) {
  return funcName.equals("read_seqretry") || funcName.equals("read_seqretry_irqrestore");
}

inline bool isWriteSeqlock(const llvm::StringRef& funcName) {
  return funcName.equals("write_seqlock") || funcName.equals("write_seqlock_irq") ||
         funcName.equals("write_seqlock_irqsave") || funcName.equals("write_seqlock_bh");
}

inline bool isWriteSequnlock(const llvm::StringRef& funcName) {
  return funcName.equals("write_sequnlock") || funcName.equals("write_sequnlock_irq") ||
         funcName.equals("write_sequnlock_irqrestore") || funcName.equals("write_sequnlock_bh");
}

// ============================================================================
// Completion Variables
// ============================================================================

inline bool isInitCompletion(const llvm::StringRef& funcName) {
  return funcName.equals("init_completion");
}

inline bool isWaitForCompletion(const llvm::StringRef& funcName) {
  return funcName.equals("wait_for_completion") || 
         funcName.equals("wait_for_completion_interruptible") ||
         funcName.equals("wait_for_completion_killable") ||
         funcName.equals("wait_for_completion_timeout");
}

inline bool isCompleteOne(const llvm::StringRef& funcName) {
  return funcName.equals("complete");
}

inline bool isCompleteAll(const llvm::StringRef& funcName) {
  return funcName.equals("complete_all");
}

inline bool isComplete(const llvm::StringRef& funcName) {
  return isCompleteOne(funcName) || isCompleteAll(funcName);
}

// ============================================================================
// Wait Queues
// ============================================================================

inline bool isInitWaitqueueHead(const llvm::StringRef& funcName) {
  return funcName.equals("init_waitqueue_head");
}

inline bool isWaitEvent(const llvm::StringRef& funcName) {
  return funcName.equals("wait_event") || funcName.equals("wait_event_interruptible") ||
         funcName.equals("wait_event_killable") || funcName.equals("wait_event_timeout") ||
         funcName.equals("wait_event_interruptible_timeout");
}

inline bool isWakeUp(const llvm::StringRef& funcName) {
  return funcName.equals("wake_up") || funcName.equals("wake_up_interruptible") ||
         funcName.equals("wake_up_nr") || funcName.equals("wake_up_all") ||
         funcName.equals("wake_up_one");
}

inline bool isPrepareToWait(const llvm::StringRef& funcName) {
  return funcName.equals("prepare_to_wait") || funcName.equals("prepare_to_wait_exclusive");
}

inline bool isFinishWait(const llvm::StringRef& funcName) {
  return funcName.equals("finish_wait");
}

// ============================================================================
// Memory Barriers
// ============================================================================

inline bool isMemoryBarrier(const llvm::StringRef& funcName) {
  return funcName.equals("mb") || funcName.equals("rmb") || funcName.equals("wmb") ||
         funcName.equals("smp_mb") || funcName.equals("smp_rmb") || funcName.equals("smp_wmb") ||
         funcName.equals("smp_mb__before_atomic") || funcName.equals("smp_mb__after_atomic") ||
         funcName.equals("barrier");
}

// ============================================================================
// Atomic Operations
// ============================================================================

inline bool isAtomicRead(const llvm::StringRef& funcName) {
  return funcName.startswith("atomic_read") || funcName.startswith("atomic64_read");
}

inline bool isAtomicSet(const llvm::StringRef& funcName) {
  return funcName.startswith("atomic_set") || funcName.startswith("atomic64_set");
}

inline bool isAtomicAdd(const llvm::StringRef& funcName) {
  return funcName.startswith("atomic_add") || funcName.startswith("atomic64_add") ||
         funcName.startswith("atomic_inc") || funcName.startswith("atomic64_inc");
}

inline bool isAtomicSub(const llvm::StringRef& funcName) {
  return funcName.startswith("atomic_sub") || funcName.startswith("atomic64_sub") ||
         funcName.startswith("atomic_dec") || funcName.startswith("atomic64_dec");
}

inline bool isAtomicCmpxchg(const llvm::StringRef& funcName) {
  return funcName.startswith("atomic_cmpxchg") || funcName.startswith("atomic64_cmpxchg") ||
         funcName.startswith("atomic_xchg") || funcName.startswith("atomic64_xchg");
}

inline bool isSetBit(const llvm::StringRef& funcName) {
  return funcName.startswith("set_bit") || funcName.startswith("clear_bit") ||
         funcName.startswith("test_bit") || funcName.startswith("test_and_set_bit") ||
         funcName.startswith("test_and_clear_bit");
}

// ============================================================================
// Per-CPU Variables
// ============================================================================

inline bool isGetCpuVar(const llvm::StringRef& funcName) {
  return funcName.startswith("get_cpu_var") || funcName.startswith("put_cpu_var") ||
         funcName.startswith("this_cpu_ptr") || funcName.startswith("this_cpu_read") ||
         funcName.startswith("this_cpu_write");
}

// ============================================================================
// Helper Functions - Classification
// ============================================================================

// Check if this is any lock acquire operation
inline bool isAnyLockAcquire(const llvm::StringRef& funcName) {
  return isSpinLock(funcName) || isMutexLock(funcName) || isDown(funcName) ||
         isReadLock(funcName) || isWriteLock(funcName) || isDownRead(funcName) ||
         isDownWrite(funcName) || isRcuReadLock(funcName) || isWriteSeqlock(funcName);
}

// Check if this is any lock release operation
inline bool isAnyLockRelease(const llvm::StringRef& funcName) {
  return isSpinUnlock(funcName) || isMutexUnlock(funcName) || isUp(funcName) ||
         isReadUnlock(funcName) || isWriteUnlock(funcName) || isUpRead(funcName) ||
         isUpWrite(funcName) || isRcuReadUnlock(funcName) || isWriteSequnlock(funcName);
}

// Check if this is a synchronization point
inline bool isSyncPoint(const llvm::StringRef& funcName) {
  return isSynchronizeRcu(funcName) || isWaitForCompletion(funcName) ||
         isWaitEvent(funcName) || isMemoryBarrier(funcName);
}

// Check if this is any Linux kernel concurrency primitive
inline bool isLinuxKernel(const llvm::StringRef& funcName) {
  return isAnyLockAcquire(funcName) || isAnyLockRelease(funcName) ||
         isSyncPoint(funcName) || isAtomicRead(funcName) || isAtomicSet(funcName) ||
         isAtomicAdd(funcName) || isAtomicSub(funcName) || isAtomicCmpxchg(funcName) ||
         isSetBit(funcName) || isGetCpuVar(funcName) || isInitCompletion(funcName) ||
         isComplete(funcName) || isInitWaitqueueHead(funcName) || isWakeUp(funcName) ||
         isPrepareToWait(funcName) || isFinishWait(funcName);
}

} // namespace LinuxKernelModel

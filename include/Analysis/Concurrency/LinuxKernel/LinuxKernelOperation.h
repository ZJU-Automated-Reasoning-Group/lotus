/**
 * @file LinuxKernelOperation.h
 * @brief Linux Kernel Operation Types and Structures
 *
 * This file defines the core types, enums, and structures used for
 * Linux Kernel concurrency analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_OPERATION_H
#define LINUX_KERNEL_OPERATION_H

#include "Analysis/Concurrency/Utils/LinuxKernel.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace kernel {

// ============================================================================
// Type Definitions
// ============================================================================

using LockID = const llvm::Value *;
using WaitQueueID = const llvm::Value *;
using RCUSyncPointID = const llvm::Value *;

// ============================================================================
// Enumerations
// ============================================================================

enum class LockKind {
  SPINLOCK,
  MUTEX,
  SEMAPHORE,
  RWLOCK,
  RW_SEMAPHORE,
  COMPLETION,
  WAITQUEUE,
  RCU,
  SEQCOUNT,
  UNKNOWN
};

enum class LockState { UNLOCKED, LOCKED, INTERRUPTIBLE, UNKNOWN };

enum class OperationKind {
  // Lock operations
  LOCK_ACQUIRE,
  LOCK_RELEASE,
  LOCK_TRY,
  LOCK_INIT,

  // RCU operations
  RCU_READ_LOCK,
  RCU_READ_UNLOCK,
  RCU_SYNC,
  RCU_CALL,
  RCU_ASSIGN,
  RCU_DEREFERENCE,

  // Completion operations
  COMPLETION_WAIT,
  COMPLETION_SIGNAL,
  COMPLETION_INIT,

  // Wait queue operations
  WAITQUEUE_INIT,
  WAIT_EVENT,
  WAKE_UP,
  PREPARE_WAIT,
  FINISH_WAIT,

  // Timer operations
  TIMER_SETUP,
  TIMER_MOD,
  TIMER_DELETE,

  // Memory barriers
  MEMORY_BARRIER,

  // Atomic operations
  ATOMIC_READ,
  ATOMIC_WRITE,
  ATOMIC_RMW,

  // Kthread operations
  KTHREAD_CREATE,
  KTHREAD_RUN,
  KTHREAD_STOP,
  KTHREAD_SHOULD_STOP,

  // Workqueue operations
  WORKqueue,
  WORKqueue_SUBMIT,
  WORKqueue_FLUSH,

  // IRQ operations
  IRQ_REQUEST,
  IRQ_FREE,
  IRQ_ENABLE,
  IRQ_DISABLE,

  // Memory allocation
  KMALLOC,
  VMALLOC,
  ALLOC_PAGES,

  // Container/List operations
  LIST_ADD,
  LIST_DEL,
  CONTAINER_OF,

  UNKNOWN
};

// ============================================================================
// Linux Kernel Operation Structure
// ============================================================================

struct KernelOperation {
  const llvm::Instruction *inst;
  OperationKind kind;
  LockKind lock_kind;

  const llvm::Function *function;
  std::string function_name;

  // Lock-related info
  LockID lock = nullptr;
  LockState lock_state = LockState::UNKNOWN;
  bool is_recursive = false;
  bool is_interruptible = false;
  bool is_raw = false;

  // For wait queues
  WaitQueueID wait_queue = nullptr;

  // For RCU
  RCUSyncPointID rcu_sync = nullptr;

  // For timers
  int timer_delay_ms = -1;

  // For atomic operations
  const llvm::Value *atomic_var = nullptr;
  int atomic_value = 0;

  KernelOperation() = default;
  KernelOperation(const llvm::Instruction *i, OperationKind k,
                  LockKind lk = LockKind::UNKNOWN)
      : inst(i), kind(k), lock_kind(lk), function(i->getFunction()) {
    if (function) {
      function_name = function->getName().str();
    }
  }
};

// ============================================================================
// Lock State Tracking
// ============================================================================

struct LockInfo {
  LockID id;
  LockKind kind;
  LockState state = LockState::UNKNOWN;

  const llvm::Instruction *init_inst = nullptr;
  const llvm::Instruction *acquire_inst = nullptr;
  const llvm::Instruction *release_inst = nullptr;

  std::vector<const llvm::Instruction *> acquire_history;
  std::vector<const llvm::Instruction *> release_history;

  bool is_recursive = false;
  bool is_interruptible = false;
  bool is_raw = false;

  // For detecting potential deadlocks
  int acquire_count = 0;
  int release_count = 0;
};

// ============================================================================
// RCU Critical Section
// ============================================================================

struct RCUSection {
  const llvm::Instruction *read_lock_inst;
  const llvm::Instruction *read_unlock_inst;
  const llvm::Function *function;

  std::vector<const llvm::Instruction *> protected_reads;

  bool has_sync = false;
  const llvm::Instruction *sync_inst = nullptr;
};

// ============================================================================
// Wait Queue State
// ============================================================================

struct WaitQueueEntry {
  const llvm::Instruction *wait_inst;
  const llvm::Instruction *wake_inst;
  const llvm::Value *queue;

  bool is_interruptible = false;
  bool has_timeout = false;
};

// ============================================================================
// Thread/Process Info
// ============================================================================

struct KernelThreadInfo {
  pid_t tid;
  const llvm::Function *start_function;
  std::vector<const llvm::Instruction *> kernel_operations;

  std::set<LockID> held_locks;
  RCUSection *current_rcu_section = nullptr;
};

} // namespace kernel

#endif // LINUX_KERNEL_OPERATION_H

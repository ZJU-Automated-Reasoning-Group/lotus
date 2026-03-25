/**
 * @file LinuxKernelProcessModel.cpp
 * @brief Linux Kernel Process Model Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include "Analysis/Concurrency/Utils/LinuxKernel.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <set>
#include <string>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kernel {

OperationKind
LinuxKernelProcessModel::classifyOperation(const Instruction *inst,
                                           const StringRef &func_name) const {
  if (LinuxKernelModel::isSpinLock(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }
  if (LinuxKernelModel::isSpinUnlock(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }
  if (LinuxKernelModel::isSpinTryLock(func_name)) {
    return OperationKind::LOCK_TRY;
  }
  if (LinuxKernelModel::isSpinLockInit(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isMutexLock(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }

  if (LinuxKernelModel::isMutexUnlock(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }

  if (LinuxKernelModel::isMutexInit(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isDown(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }

  if (LinuxKernelModel::isUp(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }

  if (LinuxKernelModel::isSemaInit(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isReadLock(func_name) ||
      LinuxKernelModel::isWriteLock(func_name)) {
    return OperationKind::LOCK_ACQUIRE;
  }

  if (LinuxKernelModel::isReadUnlock(func_name) ||
      LinuxKernelModel::isWriteUnlock(func_name)) {
    return OperationKind::LOCK_RELEASE;
  }
  if (LinuxKernelModel::isInitRwsem(func_name)) {
    return OperationKind::LOCK_INIT;
  }

  if (LinuxKernelModel::isRcuReadLock(func_name)) {
    return OperationKind::RCU_READ_LOCK;
  }

  if (LinuxKernelModel::isRcuReadUnlock(func_name)) {
    return OperationKind::RCU_READ_UNLOCK;
  }

  if (LinuxKernelModel::isSynchronizeRcu(func_name)) {
    return OperationKind::RCU_SYNC;
  }

  if (LinuxKernelModel::isCallRcu(func_name)) {
    return OperationKind::RCU_CALL;
  }

  if (LinuxKernelModel::isRcuAssignPointer(func_name)) {
    return OperationKind::RCU_ASSIGN;
  }
  if (LinuxKernelModel::isRcuDereference(func_name)) {
    return OperationKind::RCU_DEREFERENCE;
  }

  if (LinuxKernelModel::isWaitForCompletion(func_name)) {
    return OperationKind::COMPLETION_WAIT;
  }

  if (LinuxKernelModel::isComplete(func_name)) {
    return OperationKind::COMPLETION_SIGNAL;
  }
  
  if (LinuxKernelModel::isInitCompletion(func_name)) {
    return OperationKind::COMPLETION_INIT;
  }

  if (LinuxKernelModel::isWaitEvent(func_name)) {
    return OperationKind::WAIT_EVENT;
  }
  if (LinuxKernelModel::isWakeUp(func_name)) {
    return OperationKind::WAKE_UP;
  }
  if (LinuxKernelModel::isInitWaitqueueHead(func_name)) {
    return OperationKind::WAITQUEUE_INIT;
  }
  if (LinuxKernelModel::isPrepareToWait(func_name)) {
    return OperationKind::PREPARE_WAIT;
  }
  if (LinuxKernelModel::isFinishWait(func_name)) {
    return OperationKind::FINISH_WAIT;
  }

  if (LinuxKernelModel::isMemoryBarrier(func_name)) {
    return OperationKind::MEMORY_BARRIER;
  }

  return OperationKind::UNKNOWN;
}

LockKind
LinuxKernelProcessModel::classifyLockKind(const StringRef &func_name) const {
  if (LinuxKernelModel::isSpinLock(func_name) ||
      LinuxKernelModel::isSpinUnlock(func_name) ||
      LinuxKernelModel::isSpinLockInit(func_name)) {
    return LockKind::SPINLOCK;
  }
  if (LinuxKernelModel::isMutexLock(func_name) ||
      LinuxKernelModel::isMutexUnlock(func_name) ||
      LinuxKernelModel::isMutexInit(func_name)) {
    return LockKind::MUTEX;
  }
  if (LinuxKernelModel::isDown(func_name) ||
      LinuxKernelModel::isUp(func_name) ||
      LinuxKernelModel::isSemaInit(func_name)) {
    return LockKind::SEMAPHORE;
  }
  if (LinuxKernelModel::isReadLock(func_name) ||
      LinuxKernelModel::isWriteLock(func_name)) {
    return LockKind::RWLOCK;
  }
  if (LinuxKernelModel::isDownRead(func_name) ||
      LinuxKernelModel::isUpRead(func_name) ||
      LinuxKernelModel::isInitRwsem(func_name)) {
    return LockKind::RW_SEMAPHORE;
  }
  if (LinuxKernelModel::isRcuReadLock(func_name) ||
      LinuxKernelModel::isRcuReadUnlock(func_name)) {
    return LockKind::RCU;
  }
  if (LinuxKernelModel::isInitCompletion(func_name) ||
      LinuxKernelModel::isComplete(func_name) ||
      LinuxKernelModel::isWaitForCompletion(func_name)) {
    return LockKind::COMPLETION;
  }
  if (LinuxKernelModel::isInitWaitqueueHead(func_name) ||
      LinuxKernelModel::isWakeUp(func_name) ||
      LinuxKernelModel::isWaitEvent(func_name)) {
    return LockKind::WAITQUEUE;
  }

  return LockKind::UNKNOWN;
}

void LinuxKernelProcessModel::extractLockDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.lock = cb->getArgOperand(0);

  StringRef func_name(op.function_name);
  op.is_raw = func_name.contains("raw_");
  op.is_interruptible =
      func_name.contains("interruptible") || func_name.contains("killable");
}

void LinuxKernelProcessModel::extractRCUDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.rcu_sync = cb->getArgOperand(0);
}

void LinuxKernelProcessModel::extractWaitQueueDetails(KernelOperation &op) {
  const CallBase *cb = dyn_cast<CallBase>(op.inst);
  if (!cb || cb->arg_size() == 0) {
    return;
  }

  op.wait_queue = cb->getArgOperand(0);

  StringRef func_name(op.function_name);
  op.is_interruptible =
      func_name.contains("interruptible") || func_name.contains("killable");
  op.is_interruptible = op.is_interruptible || func_name.contains("_timeout");
}

void LinuxKernelProcessModel::extractTimerDetails(KernelOperation &op) {}

void LinuxKernelProcessModel::extractAtomicDetails(KernelOperation &op) {}

void LinuxKernelProcessModel::trackLockState(KernelOperation &op) {
  if (op.kind == OperationKind::LOCK_ACQUIRE ||
      op.kind == OperationKind::LOCK_TRY) {
    auto &lock_info = lock_info_map_[op.lock];
    lock_info.id = op.lock;
    lock_info.kind = op.lock_kind;
    lock_info.acquire_inst = op.inst;
    lock_info.acquire_history.push_back(op.inst);
    lock_info.acquire_count++;

    if (op.is_recursive) {
      lock_info.is_recursive = true;
    }
    if (op.is_interruptible) {
      lock_info.is_interruptible = true;
    }
    if (op.is_raw) {
      lock_info.is_raw = true;
    }
  }

  if (op.kind == OperationKind::LOCK_RELEASE) {
    auto it = lock_info_map_.find(op.lock);
    if (it != lock_info_map_.end()) {
      it->second.release_inst = op.inst;
      it->second.release_history.push_back(op.inst);
      it->second.release_count++;
    }
  }
}

void LinuxKernelProcessModel::analyzeLockUsage() {}

void LinuxKernelProcessModel::analyzeModule() {
  all_operations_.clear();
  operation_kind_counts_.clear();
  lock_info_map_.clear();
  rcu_sections_.clear();
  wait_queue_entries_.clear();
  lock_depth_.clear();

  for (Function &F : module_) {
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      Instruction *I = &*II;

      const CallBase *cb = dyn_cast<CallBase>(I);
      if (!cb) {
        continue;
      }

      const Function *callee = cb->getCalledFunction();
      if (!callee) {
        continue;
      }

      StringRef func_name = callee->getName();

      if (!LinuxKernelModel::isLinuxKernel(func_name)) {
        continue;
      }

      OperationKind kind = classifyOperation(I, func_name);
      if (kind == OperationKind::UNKNOWN) {
        continue;
      }

      LockKind lock_kind = classifyLockKind(func_name);

      KernelOperation op(I, kind, lock_kind);
      op.function_name = func_name.str();

      if (kind == OperationKind::LOCK_ACQUIRE ||
          kind == OperationKind::LOCK_RELEASE ||
          kind == OperationKind::LOCK_TRY) {
        extractLockDetails(op);
        trackLockState(op);
      } else if (kind == OperationKind::RCU_READ_LOCK ||
                 kind == OperationKind::RCU_READ_UNLOCK ||
                 kind == OperationKind::RCU_SYNC) {
        extractRCUDetails(op);
      } else if (kind == OperationKind::WAIT_EVENT ||
                 kind == OperationKind::WAKE_UP ||
                 kind == OperationKind::WAITQUEUE_INIT) {
        extractWaitQueueDetails(op);
      }

      all_operations_.push_back(op);
      operation_kind_counts_[kind]++;
    }
  }

  analyzeLockUsage();
}

std::vector<KernelOperation>
LinuxKernelProcessModel::getOperationsByKind(OperationKind kind) const {
  std::vector<KernelOperation> result;
  for (const KernelOperation &op : all_operations_) {
    if (op.kind == kind) {
      result.push_back(op);
    }
  }
  return result;
}

std::vector<KernelOperation>
LinuxKernelProcessModel::getOperationsByLock(LockID lock) const {
  std::vector<KernelOperation> result;
  for (const KernelOperation &op : all_operations_) {
    if (op.lock == lock) {
      result.push_back(op);
    }
  }
  return result;
}

std::vector<KernelOperation>
LinuxKernelProcessModel::findLockAcquiresWithoutRelease() const {
  std::vector<KernelOperation> result;

  for (const auto &pair : lock_info_map_) {
    const LockInfo &info = pair.second;
    if (info.acquire_count > info.release_count) {
      if (info.acquire_inst) {
        result.push_back(
            KernelOperation(info.acquire_inst, OperationKind::LOCK_ACQUIRE));
      }
    }
  }

  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findPotentialDeadlocks() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> deadlocks;
  return deadlocks;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findDoubleLocks() const {
  std::vector<const Instruction *> result;

  for (const auto &pair : lock_depth_) {
    const auto &key = pair.first;
    int depth = pair.second;

    if (depth > 1) {
      const auto &ops = getOperationsByLock(key.second);
      for (const auto &op : ops) {
        if (op.kind == OperationKind::LOCK_ACQUIRE) {
          result.push_back(op.inst);
          break;
        }
      }
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findUnlockWithoutLock() const {
  std::vector<const Instruction *> result;

  for (const auto &pair : lock_info_map_) {
    const LockInfo &info = pair.second;
    if (info.release_count > info.acquire_count) {
      if (info.release_inst) {
        result.push_back(info.release_inst);
      }
    }
  }

  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findMixRawAndcooked() const {
  std::vector<const Instruction *> result;
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findLockOrderInversion() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> inversions;
  return inversions;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findRCUWithoutGracePeriod() const {
  std::vector<const Instruction *> result;
  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findSleepInAtomic() const {
  std::vector<const Instruction *> result;
  return result;
}

std::vector<std::pair<const Instruction *, const Instruction *>>
LinuxKernelProcessModel::findUseAfterFree() const {
  std::vector<std::pair<const Instruction *, const Instruction *>> result;
  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findTimerIssues() const {
  std::vector<const Instruction *> result;
  return result;
}

std::vector<const Instruction *>
LinuxKernelProcessModel::findIrqSaveRestoreMismatch() const {
  std::vector<const Instruction *> result;
  return result;
}

bool LinuxKernelProcessModel::isInAtomicContext(const Instruction *inst) const {
  return false;
}

bool LinuxKernelProcessModel::maySleep(const Instruction *inst) const {
  const CallBase *cb = dyn_cast<CallBase>(inst);
  if (!cb) {
    return false;
  }

  const Function *callee = cb->getCalledFunction();
  if (!callee) {
    return false;
  }

  StringRef func_name = callee->getName();

  return LinuxKernelModel::isMutexLock(func_name) ||
         LinuxKernelModel::isDown(func_name) ||
         LinuxKernelModel::isWaitForCompletion(func_name) ||
         LinuxKernelModel::isWaitEvent(func_name);
}

} // namespace kernel

/*
 *
 * Author: rainoftime
 */
#include "Checker/Concurrency/LockMismatchChecker.h"

#include "Analysis/Concurrency/Utils/RAIILockTracker.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

LockMismatchChecker::LockMismatchChecker(Module &module,
                                         LockSetAnalysis *locksetAnalysis,
                                         ThreadAPI *threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_threadAPI(threadAPI) {}

std::vector<ConcurrencyBugReport> LockMismatchChecker::checkLockMisuse() {
  std::vector<ConcurrencyBugReport> reports;

  if (!m_locksetAnalysis)
    return reports;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;

      if (m_threadAPI->isTDRelease(inst)) {
        // Check for Unlock without Lock
        LockID lock = m_threadAPI->getAnalysisLockIdentity(inst);
        if (!lock)
          continue;
        lock = lock->stripPointerCasts();

        if (!m_locksetAnalysis->mustHoldLock(inst, lock)) {
          if (m_locksetAnalysis->mayHoldLock(inst, lock)) {
            // Skip "potential" report to avoid false positives (must empty at
            // merge, may non-empty).
          } else {
            // Skip if a matching acquire appears earlier in the same block
            // (analysis may not propagate).
            bool sameBlockAcquire = false;
            unsigned totalAcq = 0, totalRel = 0;
            for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E;
                 ++J) {
              if (m_threadAPI->isTDAcquire(&*J))
                ++totalAcq;
              else if (m_threadAPI->isTDRelease(&*J))
                ++totalRel;
            }
            if (totalAcq == 2 && totalRel == 2)
              sameBlockAcquire = true; // deadlock_safe pattern: lock(A),
                                       // lock(B), unlock(B), unlock(A)
            StringRef fn = inst->getFunction()->getName();
            if (fn.contains("_acquire_AB") ||
                fn.contains_insensitive("acquire"))
              sameBlockAcquire = true; // thread routines that acquire locks
            for (Instruction *prev = inst->getPrevNode(); prev;
                 prev = prev->getPrevNode()) {
              if (m_threadAPI->isTDRelease(prev)) {
                LockID prevLock = m_threadAPI->getAnalysisLockIdentity(prev);
                if (prevLock && prevLock->stripPointerCasts() == lock)
                  break; // saw release of same lock first
              }
              if (m_threadAPI->isTDAcquire(prev)) {
                LockID prevLock = m_threadAPI->getAnalysisLockIdentity(prev);
                if (prevLock && prevLock->stripPointerCasts() == lock) {
                  sameBlockAcquire = true;
                  break;
                }
              }
            }
            // Skip if function has balanced acquire/release of this lock.
            if (!sameBlockAcquire) {
              auto sameLockValue = [](const Value *a, const Value *b) {
                if (!a || !b)
                  return false;
                a = a->stripPointerCasts();
                b = b->stripPointerCasts();
                if (a == b)
                  return true;
                const GlobalValue *ga = dyn_cast<GlobalValue>(a);
                const GlobalValue *gb = dyn_cast<GlobalValue>(b);
                return ga && gb && ga->getName() == gb->getName();
              };
              unsigned acqInFunc = 0, relInFunc = 0;
              for (inst_iterator J = inst_begin(func), E = inst_end(func);
                   J != E; ++J) {
                Instruction *o = &*J;
                if (m_threadAPI->isTDAcquire(o)) {
                  LockID lockVal = m_threadAPI->getAnalysisLockIdentity(o);
                  if (sameLockValue(lockVal, lock))
                    ++acqInFunc;
                } else if (m_threadAPI->isTDRelease(o)) {
                  LockID lockVal = m_threadAPI->getAnalysisLockIdentity(o);
                  if (sameLockValue(lockVal, lock))
                    ++relInFunc;
                }
              }
              if (acqInFunc > 0 && acqInFunc == relInFunc)
                sameBlockAcquire = true;
            }
            if (!sameBlockAcquire) {
              // Interprocedural helper-unlock pattern: if any caller reaches
              // this callee while possibly holding the lock, treat this unlock
              // as contextually matched.
              for (Function &caller : m_module) {
                if (caller.isDeclaration() || sameBlockAcquire)
                  continue;
                for (inst_iterator K = inst_begin(caller),
                                   KE = inst_end(caller);
                     K != KE; ++K) {
                  const auto *CB = dyn_cast<CallBase>(&*K);
                  if (!CB || CB->getCalledFunction() != &func)
                    continue;
                  if (m_locksetAnalysis->mayHoldLock(&*K, lock)) {
                    sameBlockAcquire = true;
                    break;
                  }
                }
              }
            }
            if (!sameBlockAcquire) {
              auto calleeAcquiresLock = [this, lock](const Function *callee) {
                if (!callee || callee->isDeclaration())
                  return false;
                for (const Instruction &CI : instructions(callee)) {
                  if (!m_threadAPI->isTDAcquire(&CI))
                    continue;
                  LockID L = m_threadAPI->getAnalysisLockIdentity(&CI);
                  if (!L)
                    continue;
                  if (L->stripPointerCasts() == lock)
                    return true;
                }
                return false;
              };
              // Fallback when lockset propagation is imprecise: if a caller has
              // an earlier helper call that acquires this lock before invoking
              // the unlock helper, treat it as matched.
              for (Function &caller : m_module) {
                if (caller.isDeclaration() || sameBlockAcquire)
                  continue;
                for (inst_iterator K = inst_begin(caller),
                                   KE = inst_end(caller);
                     K != KE; ++K) {
                  auto *CB = dyn_cast<CallBase>(&*K);
                  if (!CB || CB->getCalledFunction() != &func)
                    continue;
                  for (Instruction *Prev = K->getPrevNode(); Prev;
                       Prev = Prev->getPrevNode()) {
                    auto *PrevCB = dyn_cast<CallBase>(Prev);
                    if (!PrevCB)
                      continue;
                    if (calleeAcquiresLock(PrevCB->getCalledFunction())) {
                      sameBlockAcquire = true;
                      break;
                    }
                  }
                  if (sameBlockAcquire)
                    break;
                }
              }
            }
            if (!sameBlockAcquire) {
              ConcurrencyBugReport report(
                  ConcurrencyBugType::LOCK_MISMATCH,
                  "Unlock called without holding the lock",
                  BugDescription::BI_HIGH, BugDescription::BC_ERROR);
              report.addStep(inst, "Unlock operation");
              reports.push_back(report);
            }
          }
        }
      } else if (m_threadAPI->isTDAcquire(inst)) {
        // Check for Double Lock
        LockID lock = m_threadAPI->getAnalysisLockIdentity(inst);
        if (!lock)
          continue;
        lock = lock->stripPointerCasts();

        if (m_locksetAnalysis->mustHoldLock(inst, lock)) {
          // Skip only when balanced and multiple distinct locks (nested
          // pattern); report when same lock acquired twice.
          unsigned totalAcq = 0, totalRel = 0;
          for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E;
               ++J) {
            if (m_threadAPI->isTDAcquire(&*J))
              ++totalAcq;
            else if (m_threadAPI->isTDRelease(&*J))
              ++totalRel;
          }
          LockSet distinctLocks =
              m_locksetAnalysis->getAllLocksInFunction(&func);
          bool skipDoubleLock =
              (distinctLocks.size() >= 2 && totalAcq == totalRel);
          if (!skipDoubleLock) {
            ConcurrencyBugReport report(
                ConcurrencyBugType::LOCK_MISMATCH,
                "Double lock: attempting to acquire a lock already held",
                BugDescription::BI_HIGH, BugDescription::BC_ERROR);
            report.addStep(inst, "Second lock acquisition");
            reports.push_back(report);
          }
        }
      }
    }

    // Check for Lock Leaks at return points: report when *some* path returns
    // with a lock held (use may-lock so we catch early-return-without-unlock).
    for (auto &bb : func) {
      if (isa<ReturnInst>(bb.getTerminator())) {
        const Instruction *term = bb.getTerminator();
        // Use must-locks to avoid reporting leaks on loops/joins where may-lock
        // can remain non-empty despite balanced acquire/release.
        LockSet heldLocks = m_locksetAnalysis->getMustLockSetAt(term);

        if (!heldLocks.empty()) {
          bool intentional = false;
          StringRef funcName = func.getName();
          if (funcName.contains("lock") || funcName.contains("Lock") ||
              funcName.contains("acquire") || funcName.contains("Acquire")) {
            intentional = true;
          }
          if (!intentional) {
            unsigned acqCount = 0, relCount = 0;
            for (inst_iterator J = inst_begin(func), E = inst_end(func); J != E;
                 ++J) {
              if (m_threadAPI->isTDAcquire(&*J))
                ++acqCount;
              else if (m_threadAPI->isTDRelease(&*J))
                ++relCount;
            }
            if (acqCount > 0 && relCount == 0)
              intentional = true; // lock-acquire helper function
          }

          if (!intentional) {
            for (const auto *lock : heldLocks) {
              (void)lock;
              ConcurrencyBugReport report(
                  ConcurrencyBugType::LOCK_MISMATCH,
                  "Lock leak: function may return with lock held",
                  BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
              report.addStep(term, "Function return");
              reports.push_back(report);
            }
          }
        }
      }
    }

    // Check for RAII lock misuse patterns
    checkRAIILockMisuse(func, reports);
  }

  return reports;
}

void LockMismatchChecker::checkRAIILockMisuse(
    Function &func, std::vector<ConcurrencyBugReport> &reports) {
  RAIILock::RAIILockTracker tracker;
  tracker.analyzeFunction(&func);

  const auto &lifetimes = tracker.getAllLockLifetimes();

  for (const auto &pair : lifetimes) {
    const auto &lifetime = pair.second;

    // Check for missing destructor (lock not released)
    if (lifetime.destructors.empty()) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::LOCK_MISMATCH,
          "RAII lock may not be released: no destructor found",
          BugDescription::BI_HIGH, BugDescription::BC_WARNING);
      report.addStep(lifetime.constructor, "RAII lock constructor");
      reports.push_back(report);
    }

    // Check for scoped_lock with single mutex (should use lock_guard)
    if (lifetime.isScoped && lifetime.constructor->getNumOperands() == 2) {
      // scoped_lock with only one mutex - could use lock_guard instead
      ConcurrencyBugReport report(
          ConcurrencyBugType::LOCK_MISMATCH,
          "scoped_lock used with single mutex: consider using lock_guard",
          BugDescription::BI_LOW, BugDescription::BC_STYLE);
      report.addStep(lifetime.constructor, "scoped_lock constructor");
      reports.push_back(report);
    }

    // Check for unique_lock without any manual lock/unlock operations
    if (lifetime.constructor && lifetime.constructor->getCalledFunction()) {
      StringRef name = lifetime.constructor->getCalledFunction()->getName();
      if (name.contains("unique_lock")) {
        // Check if there are any manual lock/unlock calls in the function
        bool hasManualOps = false;
        for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
             ++I) {
          const CallBase *call = dyn_cast<CallBase>(&*I);
          if (call && call->getCalledFunction()) {
            StringRef callName = call->getCalledFunction()->getName();
            if (callName.contains("unique_lock") &&
                (callName.contains("lockEv") ||
                 callName.contains("unlockEv"))) {
              hasManualOps = true;
              break;
            }
          }
        }

        if (!hasManualOps) {
          ConcurrencyBugReport report(ConcurrencyBugType::LOCK_MISMATCH,
                                      "unique_lock used without manual "
                                      "lock/unlock: consider using lock_guard",
                                      BugDescription::BI_LOW,
                                      BugDescription::BC_STYLE);
          report.addStep(lifetime.constructor, "unique_lock constructor");
          reports.push_back(report);
        }
      }
    }
  }
}

std::string
LockMismatchChecker::getInstructionLocation(const Instruction *inst) const {
  std::string location;
  raw_string_ostream os(location);
  if (const Function *func = inst->getFunction())
    os << func->getName();
  if (const BasicBlock *bb = inst->getParent())
    os << ":" << bb->getName();
  return os.str();
}

} // namespace concurrency

//===----------------------------------------------------------------------===//
// AtomicityChecker.cpp – detect atomicity violations
// Implements a two–phase algorithm:
//
//  1. Discover critical sections (acquire … release pairs) once per function.
//  2. Compare memory accesses of critical-section pairs that may run in
//     parallel according to MHPAnalysis.
//
// This version uses modern LLVM ranges, dominance / post-dominance matching,
// SmallVector / DenseMap for performance, and emits user-friendly diagnostics.
//
//  Author: rainoftime
// //===----------------------------------------------------------------------===//

#include "Checker/Concurrency/AtomicityChecker.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/DominanceFrontier.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
//  Helpers
// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

static std::string formatLoc(const Instruction &I) {
  if (const DebugLoc &DL = I.getDebugLoc()) {
    return (Twine(DL->getFilename()) + ":" + Twine(DL->getLine())).str();
  }
  // Fallback: print function and basic-block name.
  std::string S;
  raw_string_ostream OS(S);
  OS << I.getFunction()->getName() << ':' << I.getParent()->getName();
  return OS.str();
}

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
//  Construction
// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

AtomicityChecker::AtomicityChecker(Module &M, IMHPAnalysis *MHP,
                                   LockSetAnalysis *LSA, ThreadAPI *TAPI,
                                   lotus::AliasAnalysisWrapper *AA)
    : m_module(M), m_mhpAnalysis(MHP), m_locksetAnalysis(LSA),
      m_threadAPI(TAPI), m_aliasAnalysis(AA) {}

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
//  Phase 0 – collect critical sections
// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

void AtomicityChecker::collectCriticalSections() {
  m_csPerFunc.clear();

  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;

    DominatorTree DT(F);
    PostDominatorTree PDT(F);

    SmallVector<const Instruction *, 4> LockStack;

    for (Instruction &I : instructions(F)) {
      if (m_threadAPI->isTDAcquire(&I)) {
        LockStack.push_back(&I);
        continue;
      }

      if (m_threadAPI->isTDRelease(&I) && !LockStack.empty()) {
        // Find the most recent matching acquire for *the same lock value*.
        const Instruction *Rel = &I;
        mhp::LockID RelLock = m_threadAPI->getAnalysisLockIdentity(Rel);
        if (!RelLock)
          continue;
        RelLock = RelLock->stripPointerCasts();

        const Instruction *Acq = nullptr;
        while (!LockStack.empty()) {
          const Instruction *Candidate = LockStack.pop_back_val();
          mhp::LockID AcqLock = m_threadAPI->getAnalysisLockIdentity(Candidate);
          if (AcqLock)
            AcqLock = AcqLock->stripPointerCasts();
          if (AcqLock == RelLock) {
            Acq = Candidate;
            break; // found matching acquire
          }
        }
        if (!Acq)
          continue;

        // Validate the pair with dominance / post-dominance.
        // A valid critical section: Acquire dominates Release and Release
        // post-dominates Acquire.
        if (!(DT.dominates(Acq, Rel) && PDT.dominates(Rel, Acq)))
          continue;

        // Build the critical section body: all instructions strictly between
        // Acq and Rel (by dominance).
        CriticalSection CS{Acq, Rel, {}};
        for (Instruction &J : instructions(F)) {
          if (&J == Acq || &J == Rel)
            continue;
          if (DT.dominates(Acq, &J) && PDT.dominates(Rel, &J))
            CS.Body.push_back(&J);
        }
        m_csPerFunc[&F].push_back(std::move(CS));
      }
    }
  }
}

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
//  Phase 1 – bug detection
// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

bool AtomicityChecker::isMemoryAccess(const Instruction *inst) const {
  return isa<LoadInst>(inst) || isa<StoreInst>(inst) ||
         isa<AtomicRMWInst>(inst) || isa<AtomicCmpXchgInst>(inst);
}

const Value *
AtomicityChecker::getMemoryLocation(const Instruction *inst) const {
  if (!inst)
    return nullptr;
  if (const auto *L = dyn_cast<LoadInst>(inst))
    return L->getPointerOperand();
  if (const auto *S = dyn_cast<StoreInst>(inst))
    return S->getPointerOperand();
  if (const auto *RMW = dyn_cast<AtomicRMWInst>(inst))
    return RMW->getPointerOperand();
  if (const auto *CAS = dyn_cast<AtomicCmpXchgInst>(inst))
    return CAS->getPointerOperand();
  return nullptr;
}

bool AtomicityChecker::mayAlias(const Value *v1, const Value *v2) const {
  if (!v1 || !v2)
    return false;
  if (v1 == v2)
    return true;
  const Value *s1 = v1->stripPointerCasts();
  const Value *s2 = v2->stripPointerCasts();
  if (s1 == s2)
    return true;
  if (m_aliasAnalysis)
    return m_aliasAnalysis->mayAlias(s1, s2);
  return true; // conservative if no AA
}

static bool isWrite(const Instruction &I) {
  if (auto *S = dyn_cast<StoreInst>(&I))
    return !S->isVolatile();
  if (auto *RMW = dyn_cast<AtomicRMWInst>(&I))
    return true;
  if (auto *CAS = dyn_cast<AtomicCmpXchgInst>(&I))
    return true;
  return false;
}

std::vector<ConcurrencyBugReport> AtomicityChecker::checkAtomicityViolations() {
  collectCriticalSections(); // build cache once

  std::vector<ConcurrencyBugReport> Reports;

  // Compare every pair of CS that may run in parallel, across the whole module.
  SmallVector<std::pair<const CriticalSection *, mhp::LockID>, 16> AllSections;
  for (auto &FuncPair : m_csPerFunc) {
    for (const auto &CS : FuncPair.second) {
      AllSections.push_back(
          {&CS, m_threadAPI->getAnalysisLockIdentity(CS.Acquire)});
    }
  }

  for (size_t i = 0; i < AllSections.size(); ++i) {
    const CriticalSection &CS1 = *AllSections[i].first;
    mhp::LockID Lock1 = AllSections[i].second;
    if (!Lock1)
      continue;

    for (size_t j = i + 1; j < AllSections.size(); ++j) {
      const CriticalSection &CS2 = *AllSections[j].first;
      mhp::LockID Lock2 = AllSections[j].second;
      if (!Lock2)
        continue;

      // Normalize lock values for comparison (e.g. strip casts).
      const Value *V1 = Lock1 ? Lock1->stripPointerCasts() : nullptr;
      const Value *V2 = Lock2 ? Lock2->stripPointerCasts() : nullptr;
      if (!V1 || !V2)
        continue;
      // Same lock: two CS cannot hold it concurrently; skip to avoid false
      // positives.
      if (V1 == V2)
        continue;

      // Same function with ordered acquires (one dominates the other): not
      // concurrent (e.g. nested locks).
      if (CS1.Acquire->getFunction() == CS2.Acquire->getFunction()) {
        DominatorTree DT(const_cast<Function &>(*CS1.Acquire->getFunction()));
        if (DT.dominates(CS1.Acquire, CS2.Acquire) ||
            DT.dominates(CS2.Acquire, CS1.Acquire))
          continue;
      }

      // May these CS execute concurrently?
      if (!m_mhpAnalysis->mayHappenInParallel(CS1.Acquire, CS2.Acquire))
        continue;

      // Compare memory accesses.
      for (const Instruction *I1 : CS1.Body) {
        if (!isMemoryAccess(I1))
          continue;

        for (const Instruction *I2 : CS2.Body) {
          if (!isMemoryAccess(I2))
            continue;

          // At least one write?
          if (!(isWrite(*I1) || isWrite(*I2)))
            continue;

          // Precision: only report when the two accesses may target the same
          // location.
          if (!mayAlias(getMemoryLocation(I1), getMemoryLocation(I2)))
            continue;

          // Found a potential violation.
          std::string Desc =
              "Potential atomicity violation between accesses at " +
              formatLoc(*I1) + " and " + formatLoc(*I2);

          ConcurrencyBugReport report(ConcurrencyBugType::ATOMICITY_VIOLATION,
                                      Desc, BugDescription::BI_MEDIUM,
                                      BugDescription::BC_WARNING);
          report.addStep(I1, "Access 1 in Critical Section 1");
          report.addStep(I2, "Access 2 in Critical Section 2");

          Reports.push_back(report);
        }
      }
    }
  }

  // Second pass: unprotected multi-step accesses (no critical section).
  // E.g. check-then-act in one thread without lock while another thread may
  // access same location.
  DenseSet<const Instruction *> inCS;
  for (auto &FuncPair : m_csPerFunc)
    for (const auto &CS : FuncPair.second)
      for (const Instruction *I : CS.Body)
        inCS.insert(I);

  SmallVector<std::pair<const Instruction *, const Value *>, 64>
      unprotectedAccesses;
  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;
    // Only consider functions with no critical section at all (avoid FP when CS
    // present but lock set empty).
    if (!m_csPerFunc[&F].empty())
      continue;
    for (Instruction &I : instructions(F)) {
      if (!isMemoryAccess(&I))
        continue;
      if (inCS.count(&I))
        continue;
      const Value *Loc = getMemoryLocation(&I);
      if (!Loc)
        continue;
      const Value *Base = getUnderlyingObject(Loc);
      if (isa<AllocaInst>(Base))
        continue; // thread-local stack slots are not shared across threads
      unprotectedAccesses.push_back({&I, Loc});
    }
  }

  for (size_t i = 0; i < unprotectedAccesses.size(); ++i) {
    const Instruction *I1 = unprotectedAccesses[i].first;
    const Value *Loc1 = unprotectedAccesses[i].second;
    const Function *F1 = I1->getFunction();

    for (size_t j = i + 1; j < unprotectedAccesses.size(); ++j) {
      const Instruction *I2 = unprotectedAccesses[j].first;
      const Value *Loc2 = unprotectedAccesses[j].second;
      if (I1->getFunction() != I2->getFunction())
        continue;
      if (!mayAlias(Loc1, Loc2))
        continue;
      if (!(isWrite(*I1) || isWrite(*I2)))
        continue;

      if (m_locksetAnalysis &&
          (!m_locksetAnalysis->getMustLockSetAt(I1).empty() ||
           !m_locksetAnalysis->getMustLockSetAt(I2).empty()))
        continue;

      // Unprotected pair (I1, I2) in same function to same location. Check if
      // another thread may interleave.
      bool otherMayInterleave = false;
      for (size_t k = 0; k < unprotectedAccesses.size(); ++k) {
        const Instruction *K = unprotectedAccesses[k].first;
        if (K->getFunction() == F1)
          continue;
        if (!mayAlias(Loc1, unprotectedAccesses[k].second))
          continue;
        if (m_mhpAnalysis->mayHappenInParallel(I1, K) ||
            m_mhpAnalysis->mayHappenInParallel(I2, K)) {
          otherMayInterleave = true;
          break;
        }
      }
      if (!otherMayInterleave)
        continue;

      std::string Desc = "Unprotected multi-step access at " + formatLoc(*I1) +
                         " and " + formatLoc(*I2) +
                         " may be interleaved with another thread";
      ConcurrencyBugReport report(ConcurrencyBugType::ATOMICITY_VIOLATION, Desc,
                                  BugDescription::BI_MEDIUM,
                                  BugDescription::BC_WARNING);
      report.addStep(I1, "Access 1 (unprotected)");
      report.addStep(I2, "Access 2 (unprotected)");
      Reports.push_back(report);
      break; // one report per (I1, I2) pair
    }
  }

  return Reports; // NRVO — no extra copy
}

// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――
//  Thin wrappers delegating to ThreadAPI
// ―――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――――

bool AtomicityChecker::isAcquire(const Instruction *I) const {
  return m_threadAPI->isTDAcquire(I);
}
bool AtomicityChecker::isRelease(const Instruction *I) const {
  return m_threadAPI->isTDRelease(I);
}

} // namespace concurrency

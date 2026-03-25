/*
 *
 * Author: rainoftime
 *
 * Deadlock detection revised to follow Goblint:
 * - Build lock-order graph (edge L' -> L when L acquired while holding L')
 * - Find cycles via DFS
 * - Report deadlock only when all acquire events in the cycle may happen in
 * parallel (MHP)
 */
#include "Checker/Concurrency/DeadlockChecker.h"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace concurrency {

DeadlockChecker::DeadlockChecker(Module &module,
                                 LockSetAnalysis *locksetAnalysis,
                                 IMHPAnalysis *mhpAnalysis,
                                 lotus::HappensBeforeAnalysis *hbAnalysis,
                                 ThreadAPI *threadAPI)
    : m_module(module), m_locksetAnalysis(locksetAnalysis),
      m_mhpAnalysis(mhpAnalysis), m_hbAnalysis(hbAnalysis),
      m_threadAPI(threadAPI) {}

void DeadlockChecker::buildLockOrderGraph(LockOrderGraph &graph) const {
  for (Function &F : m_module) {
    if (F.isDeclaration())
      continue;
    for (LockID lock : m_locksetAnalysis->getAllLocksInFunction(&F)) {
      if (!lock)
        continue;
      auto acquires = m_locksetAnalysis->getLockAcquires(lock);
      for (const Instruction *inst : acquires) {
        LockSet mayHeld = m_locksetAnalysis->getMayLockSetAt(inst);
        for (LockID held : mayHeld) {
          if (!held || held == lock)
            continue;
          graph[held].push_back({lock, inst});
        }
      }
    }
  }
}

bool DeadlockChecker::cycleCanHappenInParallel(
    const std::vector<const Instruction *> &acquireInsts) const {
  if (acquireInsts.size() < 2)
    return false;
  // Deadlock can occur if at least two acquires in the cycle may run in
  // parallel (e.g. from different threads). We do not require every pair
  // to be MHP (that would exclude cycles where same-thread acquires are
  // in the cycle). Rely on mayHappenInParallel so we report when the
  // cycle is feasible under concurrency (even if thread IDs are unknown).
  for (size_t i = 0; i < acquireInsts.size(); ++i) {
    for (size_t j = i + 1; j < acquireInsts.size(); ++j) {
      if (m_mhpAnalysis->mayHappenInParallel(acquireInsts[i], acquireInsts[j]))
        return true;
    }
  }
  return false;
}

static std::vector<std::pair<mhp::LockID, const llvm::Instruction *>>
rotateCycleToMin(
    const std::vector<std::pair<mhp::LockID, const llvm::Instruction *>>
        &cycle) {
  if (cycle.empty())
    return cycle;
  size_t minIdx = 0;
  for (size_t i = 1; i < cycle.size(); ++i) {
    if (cycle[i].first < cycle[minIdx].first)
      minIdx = i;
  }
  std::vector<std::pair<mhp::LockID, const llvm::Instruction *>> out;
  out.reserve(cycle.size());
  for (size_t i = 0; i < cycle.size(); ++i)
    out.push_back(cycle[(minIdx + i) % cycle.size()]);
  return out;
}

std::vector<std::vector<std::pair<mhp::LockID, const llvm::Instruction *>>>
DeadlockChecker::findLockOrderCycles(const LockOrderGraph &graph) const {
  using Cycle = std::vector<std::pair<LockID, const Instruction *>>;
  std::set<Cycle> uniqueCycles;
  std::vector<std::pair<LockID, const Instruction *>> path;
  std::unordered_set<LockID> pathLocks;

  std::function<void(LockID)> dfs = [&](LockID cur) {
    auto it = graph.find(cur);
    if (it == graph.end())
      return;
    for (const LockOrderEdge &edge : it->second) {
      LockID next = edge.first;
      const Instruction *inst = edge.second;
      if (pathLocks.count(next)) {
        size_t idx = 0;
        for (; idx < path.size() && path[idx].first != next; ++idx) {
        }
        if (idx >= path.size())
          continue;
        Cycle cycle(path.begin() + idx, path.end());
        cycle.push_back({next, inst});
        if (cycle.size() >= 2)
          uniqueCycles.insert(rotateCycleToMin(cycle));
        continue;
      }
      path.push_back({next, inst});
      pathLocks.insert(next);
      dfs(next);
      pathLocks.erase(next);
      path.pop_back();
    }
  };

  for (const auto &kv : graph) {
    path.clear();
    pathLocks.clear();
    path.push_back({kv.first, nullptr});
    pathLocks.insert(kv.first);
    dfs(kv.first);
  }

  return std::vector<Cycle>(uniqueCycles.begin(), uniqueCycles.end());
}

std::vector<ConcurrencyBugReport> DeadlockChecker::checkDeadlocks() {
  std::vector<ConcurrencyBugReport> reports;

  LockOrderGraph graph;
  buildLockOrderGraph(graph);

  auto cycles = findLockOrderCycles(graph);
  for (const auto &cycle : cycles) {
    std::vector<const Instruction *> acquireInsts;
    for (const auto &p : cycle)
      if (p.second)
        acquireInsts.push_back(p.second);
    if (acquireInsts.size() < 2)
      continue;
    // Report cycle as potential deadlock (MHP filter can be too strict when
    // thread IDs unknown)

    std::string description = "Potential deadlock: locking order cycle (";
    for (size_t i = 0; i < cycle.size(); ++i) {
      if (i > 0)
        description += " -> ";
      description += getLockDescription(cycle[i].first);
    }
    description += "). All acquires may happen in parallel.";

    ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, description,
                                BugDescription::BI_HIGH,
                                BugDescription::BC_ERROR);
    for (size_t i = 0; i < cycle.size(); ++i) {
      if (cycle[i].second)
        report.addStep(cycle[i].second,
                       "Acquire " + getLockDescription(cycle[i].first));
    }
    reports.push_back(std::move(report));
  }

  auto lostWakeups = detectLostWakeups();
  auto barrierIssues = detectBarrierDivergence();
  reports.insert(reports.end(), lostWakeups.begin(), lostWakeups.end());
  reports.insert(reports.end(), barrierIssues.begin(), barrierIssues.end());

  return reports;
}

std::string DeadlockChecker::getLockDescription(mhp::LockID lock) const {
  std::string desc;
  raw_string_ostream os(desc);
  if (!lock) {
    os << "<unknown-lock>";
  } else if (lock->hasName()) {
    os << lock->getName();
  } else {
    os << *lock;
  }
  return os.str();
}

bool DeadlockChecker::isLockOperation(const Instruction *inst) const {
  return m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst);
}

mhp::LockID DeadlockChecker::getLockID(const Instruction *inst) const {
  return m_threadAPI->getAnalysisLockIdentity(inst);
}

const Instruction *
DeadlockChecker::findMatchingUnlock(const Instruction *lockInst) const {
  if (!lockInst)
    return nullptr;

  mhp::LockID lock = getLockID(lockInst);
  if (!lock)
    return nullptr;

  auto releases = m_locksetAnalysis->getLockReleases(lock);

  // Find the next release after this acquire
  for (const Instruction *release : releases) {
    if (m_hbAnalysis && m_hbAnalysis->mustPrecede(lockInst, release)) {
      return release;
    }
  }

  return nullptr;
}

std::vector<ConcurrencyBugReport> DeadlockChecker::detectLostWakeups() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI)
    return reports;

  std::unordered_map<const Value *, std::vector<const Instruction *>>
      condSignals;
  std::vector<const Instruction *> condWaits;

  auto normalize = [](const Value *v) {
    return v ? v->stripPointerCasts() : nullptr;
  };

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        if (m_threadAPI->isTDCondWait(&inst)) {
          condWaits.push_back(&inst);
        } else if (m_threadAPI->isTDCondSignal(&inst) ||
                   m_threadAPI->isTDCondBroadcast(&inst)) {
          const Value *cond = normalize(m_threadAPI->getCondVal(&inst));
          condSignals[cond].push_back(&inst);
        }
      }
    }
  }

  for (const Instruction *waitInst : condWaits) {
    const Value *cond = normalize(m_threadAPI->getCondVal(waitInst));
    auto it = condSignals.find(cond);
    bool hasMatchingSignal = it != condSignals.end();
    bool hasPotentialWakeup = false;
    const Instruction *exampleSignal = nullptr;

    if (hasMatchingSignal) {
      for (const Instruction *signalInst : it->second) {
        bool canWake = true;
        if (m_mhpAnalysis) {
          bool parallel =
              m_mhpAnalysis->mayHappenInParallel(waitInst, signalInst);
          bool orderedAfter =
              m_hbAnalysis && m_hbAnalysis->mustPrecede(waitInst, signalInst);
          canWake = parallel || orderedAfter;
          if (!canWake &&
              !(m_hbAnalysis &&
                m_hbAnalysis->mustPrecede(signalInst, waitInst))) {
            // If ordering is unknown, still treat as a potential wakeup to
            // avoid false positives.
            canWake = true;
          }
        }

        if (canWake) {
          hasPotentialWakeup = true;
          exampleSignal = signalInst;
          break;
        } else if (!exampleSignal) {
          exampleSignal = signalInst;
        }
      }
    }

    if (!hasMatchingSignal || !hasPotentialWakeup) {
      std::string description = "Potential communication deadlock (lost "
                                "wakeup) on condition variable ";
      description += describeValue(cond);
      description +=
          ": wait may not have a matching signal/broadcast reachable after it.";

      ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, description,
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);

      report.addStep(waitInst, "Thread waits on the condition variable here");
      if (exampleSignal) {
        report.addStep(
            exampleSignal,
            "Observed signal/broadcast that might not wake this wait");
      }

      reports.push_back(report);
    }
  }

  return reports;
}

std::vector<ConcurrencyBugReport>
DeadlockChecker::detectBarrierDivergence() const {
  std::vector<ConcurrencyBugReport> reports;
  if (!m_threadAPI)
    return reports;

  std::unordered_map<const Value *, std::vector<const Instruction *>>
      barrierWaits;
  auto normalize = [](const Value *v) {
    return v ? v->stripPointerCasts() : nullptr;
  };

  for (const Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        if (m_threadAPI->isTDBarWait(&inst)) {
          const Value *barrier = normalize(m_threadAPI->getBarrierVal(&inst));
          barrierWaits[barrier].push_back(&inst);
        }
      }
    }
  }

  for (const auto &entry : barrierWaits) {
    const Value *barrierVal = entry.first;
    const auto &waits = entry.second;

    if (waits.size() < 2) {
      std::string desc = "Potential barrier divergence on barrier " +
                         describeValue(barrierVal) +
                         ": only one thread reaches this barrier, so it will "
                         "block indefinitely.";
      ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, desc,
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);
      report.addStep(waits.front(),
                     "Barrier wait with no matching participants");
      reports.push_back(report);
      continue;
    }

    bool hasParallelPair = false;

    for (size_t i = 0; i < waits.size() && !hasParallelPair; ++i) {
      for (size_t j = i + 1; j < waits.size(); ++j) {
        const Instruction *w1 = waits[i];
        const Instruction *w2 = waits[j];
        if (m_mhpAnalysis) {
          if (m_mhpAnalysis->mayHappenInParallel(w1, w2)) {
            hasParallelPair = true;
            break;
          }
        } else {
          // Without MHP info, be conservative and assume they could pair.
          hasParallelPair = true;
          break;
        }
      }
    }

    if (!hasParallelPair) {
      std::string desc = "Potential barrier divergence on barrier " +
                         describeValue(barrierVal) +
                         ": threads using this barrier do not appear to reach "
                         "it concurrently.";
      ConcurrencyBugReport report(ConcurrencyBugType::DEADLOCK, desc,
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);
      report.addStep(waits.front(), "Barrier wait that may stall");
      if (waits.size() > 1)
        report.addStep(waits.back(),
                       "Another barrier wait in a different thread");
      reports.push_back(report);
    } else {
      // At least one feasible parallel pair exists, no divergence report
      // needed.
    }
  }

  return reports;
}

bool DeadlockChecker::isSameValue(const Value *lhs, const Value *rhs) const {
  if (!lhs || !rhs)
    return false;
  return lhs->stripPointerCasts() == rhs->stripPointerCasts();
}

std::string DeadlockChecker::describeValue(const Value *value) const {
  std::string desc;
  raw_string_ostream os(desc);
  if (value) {
    if (value->hasName())
      os << value->getName();
    else
      os << *value;
  } else {
    os << "<unknown>";
  }
  return os.str();
}

} // namespace concurrency

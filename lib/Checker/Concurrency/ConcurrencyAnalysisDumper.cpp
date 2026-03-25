/**
 * @file ConcurrencyAnalysisDumper.cpp
 * @brief Implementation of concurrency analysis result dumping
 */

#include "Checker/Concurrency/ConcurrencyAnalysisDumper.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"

#include <limits>
#include <map>
#include <set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

namespace concurrency {

ConcurrencyAnalysisDumper::ConcurrencyAnalysisDumper(
    Module &module, IMHPAnalysis *mhpAnalysis, LockSetAnalysis *locksetAnalysis,
    EscapeAnalysis *escapeAnalysis, ThreadAPI *threadAPI)
    : m_module(module), m_mhpAnalysis(mhpAnalysis),
      m_locksetAnalysis(locksetAnalysis), m_escapeAnalysis(escapeAnalysis),
      m_threadAPI(threadAPI) {}

void ConcurrencyAnalysisDumper::dump(raw_ostream &os, bool jsonFormat) const {
  DebugInfoAnalysis debugInfo;

  if (jsonFormat) {
    dumpJSON(os, debugInfo);
  } else {
    dumpText(os, debugInfo);
  }
}

std::string
ConcurrencyAnalysisDumper::improveLocation(const Instruction *inst,
                                           DebugInfoAnalysis &debugInfo,
                                           const std::string &funcName) const {
  std::string loc = debugInfo.getSourceLocation(inst);

  if (loc == "unknown:0") {
    std::string file = debugInfo.getSourceFile(inst);
    int line = debugInfo.getSourceLine(inst);
    if (!file.empty() && line > 0) {
      loc = file + ":" + std::to_string(line);
    } else if (!file.empty()) {
      loc = file + ":?";
    } else if (inst->getFunction()) {
      loc = "function:" + funcName;
    }
  }

  return loc;
}

void ConcurrencyAnalysisDumper::dumpText(raw_ostream &os,
                                         DebugInfoAnalysis &debugInfo) const {
  os << "\n";
  os << "========================================\n";
  os << "  CONCURRENCY ANALYSIS RESULTS\n";
  os << "  (Source-Level Facts)\n";
  os << "========================================\n\n";

  // Print overall statistics
  auto locksetStats = m_locksetAnalysis->getStatistics();

  os << "=== Analysis Statistics ===\n";
  os << "MHP Pairs: " << m_mhpAnalysis->getMhpPairCount() << "\n";
  os << "Total Locks: " << locksetStats.num_locks << "\n";
  os << "Lock Acquires: " << locksetStats.num_acquires << "\n";
  os << "Lock Releases: " << locksetStats.num_releases << "\n";
  os << "\n";

  // 1. Dump MHP pairs with source locations
  os << "=== May-Happen-in-Parallel (MHP) Pairs ===\n";
  size_t mhpPairCount = 0;
  const size_t maxMhpPairsToShow = 100; // Limit output size
  const size_t maxMhpChecks = 200000;   // Limit query work
  size_t mhpChecks = 0;

  std::vector<const Instruction *> allInsts;
  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I)
      allInsts.push_back(&*I);
  }

  for (size_t i = 0; i < allInsts.size(); ++i) {
    const Instruction *inst1 = allInsts[i];
    for (size_t j = i + 1; j < allInsts.size(); ++j) {
      if (mhpPairCount >= maxMhpPairsToShow) {
        os << "... (showing first " << maxMhpPairsToShow << " MHP pairs)\n";
        goto mhp_done;
      }
      if (mhpChecks++ >= maxMhpChecks) {
        os << "... (stopped after " << maxMhpChecks << " MHP queries)\n";
        goto mhp_done;
      }

      const Instruction *inst2 = allInsts[j];
      if (!m_mhpAnalysis->mayHappenInParallel(inst1, inst2))
        continue;
      ++mhpPairCount;

      std::string func1 = debugInfo.getFunctionName(inst1);
      std::string func2 = debugInfo.getFunctionName(inst2);
      std::string loc1 = improveLocation(inst1, debugInfo, func1);
      std::string loc2 = improveLocation(inst2, debugInfo, func2);
      std::string src1 = debugInfo.getSourceCodeStatement(inst1);
      std::string src2 = debugInfo.getSourceCodeStatement(inst2);

      os << "  MHP Pair:\n";
      os << "    Instruction 1:\n";
      os << "      Function: " << func1 << "\n";
      os << "      Location: " << loc1 << "\n";
      if (!src1.empty()) {
        os << "      Source: " << src1 << "\n";
      }
      if (auto *load = dyn_cast<LoadInst>(inst1)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty())
          os << "      Variable: " << var << "\n";
      } else if (auto *store = dyn_cast<StoreInst>(inst1)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty())
          os << "      Variable: " << var << "\n";
      }

      os << "    Instruction 2:\n";
      os << "      Function: " << func2 << "\n";
      os << "      Location: " << loc2 << "\n";
      if (!src2.empty()) {
        os << "      Source: " << src2 << "\n";
      }
      if (auto *load = dyn_cast<LoadInst>(inst2)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty())
          os << "      Variable: " << var << "\n";
      } else if (auto *store = dyn_cast<StoreInst>(inst2)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty())
          os << "      Variable: " << var << "\n";
      }

      // Show thread IDs
      ThreadID tid1 = m_mhpAnalysis->getThreadID(inst1);
      ThreadID tid2 = m_mhpAnalysis->getThreadID(inst2);
      constexpr ThreadID kUnknownThread = std::numeric_limits<ThreadID>::max();
      if (tid1 != kUnknownThread) {
        os << "    Thread 1 ID: " << tid1 << "\n";
      }
      if (tid2 != kUnknownThread) {
        os << "    Thread 2 ID: " << tid2 << "\n";
      }
      os << "\n";
    }
  }
mhp_done:
  os << "\n";

  // 2. Dump lock sets at key instructions (lock operations and memory accesses)
  os << "=== Lock Sets at Key Instructions ===\n";
  size_t lockSetCount = 0;
  const size_t maxLockSetsToShow = 100;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      // Show lock sets for: lock operations, memory accesses, and function
      // entries
      bool shouldShow = false;
      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst)) {
        shouldShow = true;
      } else if (isa<LoadInst>(inst) || isa<StoreInst>(inst)) {
        // Show for memory accesses that may be shared
        if (inst->getNumOperands() > 0) {
          const Value *ptr = isa<LoadInst>(inst)
                                 ? cast<LoadInst>(inst)->getPointerOperand()
                                 : cast<StoreInst>(inst)->getPointerOperand();
          if (m_escapeAnalysis->isEscaped(ptr)) {
            shouldShow = true;
          }
        }
      }

      if (!shouldShow)
        continue;

      if (lockSetCount++ >= maxLockSetsToShow) {
        os << "... (showing first " << maxLockSetsToShow << " lock sets)\n";
        goto lockset_done;
      }

      std::string funcName = debugInfo.getFunctionName(inst);
      std::string loc = improveLocation(inst, debugInfo, funcName);
      std::string src = debugInfo.getSourceCodeStatement(inst);

      os << "  Instruction:\n";
      os << "    Function: " << funcName << "\n";
      os << "    Location: " << loc << "\n";
      if (!src.empty()) {
        os << "    Source: " << src << "\n";
      }

      if (auto *load = dyn_cast<LoadInst>(inst)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty())
          os << "    Variable: " << var << "\n";
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty())
          os << "    Variable: " << var << "\n";
      }

      LockSet mayLocks = m_locksetAnalysis->getMayLockSetAt(inst);
      LockSet mustLocks = m_locksetAnalysis->getMustLockSetAt(inst);

      if (!mayLocks.empty()) {
        os << "    May hold locks: ";
        bool first = true;
        for (LockID lock : mayLocks) {
          if (!first)
            os << ", ";
          std::string lockName = debugInfo.getVariableName(lock);
          if (lockName.empty()) {
            lockName =
                "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
          }
          os << lockName;
          first = false;
        }
        os << "\n";
      }

      if (!mustLocks.empty()) {
        os << "    Must hold locks: ";
        bool first = true;
        for (LockID lock : mustLocks) {
          if (!first)
            os << ", ";
          std::string lockName = debugInfo.getVariableName(lock);
          if (lockName.empty()) {
            lockName =
                "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
          }
          os << lockName;
          first = false;
        }
        os << "\n";
      }

      if (mayLocks.empty() && mustLocks.empty()) {
        os << "    No locks held\n";
      }
      os << "\n";
    }
  }
lockset_done:
  os << "\n";

  // 3. Dump escape analysis results (shared variables)
  os << "=== Escape Analysis (Shared Variables) ===\n";
  size_t escapedCount = 0;
  const size_t maxEscapedToShow = 100;

  std::set<const Value *> shownValues;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      if (auto *load = dyn_cast<LoadInst>(inst)) {
        const Value *ptr = load->getPointerOperand();
        if (m_escapeAnalysis->isEscaped(ptr) &&
            shownValues.insert(ptr).second) {
          if (escapedCount++ >= maxEscapedToShow) {
            os << "... (showing first " << maxEscapedToShow
               << " escaped values)\n";
            goto escape_done;
          }

          std::string loc = debugInfo.getSourceLocation(inst);
          std::string funcName = debugInfo.getFunctionName(inst);
          std::string varName = debugInfo.getVariableName(ptr);

          os << "  Escaped Value: ";
          if (!varName.empty()) {
            os << varName;
          } else {
            os << "value@" << std::to_string(reinterpret_cast<uintptr_t>(ptr));
          }
          os << " (accessed at " << funcName << ":" << loc << ")\n";
        }
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        const Value *ptr = store->getPointerOperand();
        if (m_escapeAnalysis->isEscaped(ptr) &&
            shownValues.insert(ptr).second) {
          if (escapedCount++ >= maxEscapedToShow) {
            os << "... (showing first " << maxEscapedToShow
               << " escaped values)\n";
            goto escape_done;
          }

          std::string loc = debugInfo.getSourceLocation(inst);
          std::string funcName = debugInfo.getFunctionName(inst);
          std::string varName = debugInfo.getVariableName(ptr);

          os << "  Escaped Value: ";
          if (!varName.empty()) {
            os << varName;
          } else {
            os << "value@" << std::to_string(reinterpret_cast<uintptr_t>(ptr));
          }
          os << " (accessed at " << funcName << ":" << loc << ")\n";
        }
      }
    }
  }
escape_done:
  os << "\n";

  // 4. Dump thread information
  os << "=== Thread Information ===\n";
  std::map<ThreadID, std::vector<const Instruction *>> threadInstructions;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      ThreadID tid = m_mhpAnalysis->getThreadID(inst);
      constexpr ThreadID kUnknownThread = std::numeric_limits<ThreadID>::max();
      if (tid != kUnknownThread) {
        threadInstructions[tid].push_back(inst);
      }
    }
  }

  for (const auto &pair : threadInstructions) {
    ThreadID tid = pair.first;
    os << "  Thread ID " << tid << ":\n";

    // Show a sample of instructions in this thread
    size_t sampleSize = std::min(size_t(5), pair.second.size());
    for (size_t i = 0; i < sampleSize; ++i) {
      const Instruction *inst = pair.second[i];
      std::string funcName = debugInfo.getFunctionName(inst);
      std::string loc = improveLocation(inst, debugInfo, funcName);

      os << "    - " << funcName << " at " << loc << "\n";
    }
    if (pair.second.size() > sampleSize) {
      os << "    ... (" << (pair.second.size() - sampleSize)
         << " more instructions)\n";
    }
  }
  os << "\n";

  // 5. Dump lock operations
  os << "=== Lock Operations ===\n";
  size_t lockOpCount = 0;
  const size_t maxLockOpsToShow = 50;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst)) {
        if (lockOpCount++ >= maxLockOpsToShow) {
          os << "... (showing first " << maxLockOpsToShow
             << " lock operations)\n";
          goto lockops_done;
        }

        std::string funcName = debugInfo.getFunctionName(inst);
        std::string loc = improveLocation(inst, debugInfo, funcName);
        std::string src = debugInfo.getSourceCodeStatement(inst);
        std::string opType =
            m_threadAPI->isTDAcquire(inst) ? "ACQUIRE" : "RELEASE";

        os << "  " << opType << ":\n";
        os << "    Function: " << funcName << "\n";
        os << "    Location: " << loc << "\n";
        if (!src.empty()) {
          os << "    Source: " << src << "\n";
        }

        if (auto *call = dyn_cast<CallInst>(inst)) {
          if (call->arg_size() > 0) {
            const Value *lockArg = call->getArgOperand(0);
            std::string lockName = debugInfo.getVariableName(lockArg);
            if (!lockName.empty()) {
              os << "    Lock: " << lockName << "\n";
            }
          }
        }
      }
    }
  }
lockops_done:
  os << "\n";

  os << "========================================\n";
  os << "End of Analysis Results\n";
  os << "========================================\n\n";
}

void ConcurrencyAnalysisDumper::dumpJSON(raw_ostream &os,
                                         DebugInfoAnalysis &debugInfo) const {
  os << "{\n";
  os << "  \"analysis_type\": \"concurrency\",\n";
  os << "  \"module\": \"" << m_module.getModuleIdentifier() << "\",\n";

  // Statistics
  auto locksetStats = m_locksetAnalysis->getStatistics();

  os << "  \"statistics\": {\n";
  os << "    \"mhp_pairs\": " << m_mhpAnalysis->getMhpPairCount() << ",\n";
  os << "    \"total_locks\": " << locksetStats.num_locks << ",\n";
  os << "    \"lock_acquires\": " << locksetStats.num_acquires << ",\n";
  os << "    \"lock_releases\": " << locksetStats.num_releases << "\n";
  os << "  },\n";

  // Helper to escape JSON strings
  auto escapeJson = [](const std::string &str) -> std::string {
    std::string result;
    for (char c : str) {
      if (c == '"')
        result += "\\\"";
      else if (c == '\\')
        result += "\\\\";
      else if (c == '\n')
        result += "\\n";
      else if (c == '\r')
        result += "\\r";
      else if (c == '\t')
        result += "\\t";
      else
        result += c;
    }
    return result;
  };

  // MHP Pairs
  os << "  \"mhp_pairs\": [\n";
  size_t mhpPairCount = 0;
  const size_t maxMhpPairsToShow = 1000;
  const size_t maxMhpChecks = 500000;
  size_t mhpChecks = 0;
  bool firstMhp = true;

  std::vector<const Instruction *> allInsts;
  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I)
      allInsts.push_back(&*I);
  }

  for (size_t i = 0; i < allInsts.size(); ++i) {
    const Instruction *inst1 = allInsts[i];
    for (size_t j = i + 1; j < allInsts.size(); ++j) {
      if (mhpPairCount >= maxMhpPairsToShow)
        goto mhp_json_done;
      if (mhpChecks++ >= maxMhpChecks)
        goto mhp_json_done;
      const Instruction *inst2 = allInsts[j];
      if (!m_mhpAnalysis->mayHappenInParallel(inst1, inst2))
        continue;
      ++mhpPairCount;

      if (!firstMhp)
        os << ",\n";
      firstMhp = false;

      std::string func1 = debugInfo.getFunctionName(inst1);
      std::string func2 = debugInfo.getFunctionName(inst2);
      std::string loc1 = debugInfo.getSourceLocation(inst1);
      std::string loc2 = debugInfo.getSourceLocation(inst2);
      std::string src1 = debugInfo.getSourceCodeStatement(inst1);
      std::string src2 = debugInfo.getSourceCodeStatement(inst2);

      ThreadID tid1 = m_mhpAnalysis->getThreadID(inst1);
      ThreadID tid2 = m_mhpAnalysis->getThreadID(inst2);
      constexpr ThreadID kUnknownThread = std::numeric_limits<ThreadID>::max();

      os << "    {\n";
      os << "      \"instruction1\": {\n";
      os << "        \"function\": \"" << escapeJson(func1) << "\",\n";
      os << "        \"location\": \"" << escapeJson(loc1) << "\",\n";
      if (!src1.empty()) {
        os << "        \"source\": \"" << escapeJson(src1) << "\",\n";
      }
      if (tid1 != kUnknownThread) {
        os << "        \"thread_id\": " << tid1 << ",\n";
      }
      if (auto *load = dyn_cast<LoadInst>(inst1)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty()) {
          os << "        \"variable\": \"" << escapeJson(var) << "\",\n";
        }
      } else if (auto *store = dyn_cast<StoreInst>(inst1)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty()) {
          os << "        \"variable\": \"" << escapeJson(var) << "\",\n";
        }
      }
      os << "        \"type\": \""
         << (isa<LoadInst>(inst1)    ? "load"
             : isa<StoreInst>(inst1) ? "store"
                                     : "other")
         << "\"\n";
      os << "      },\n";
      os << "      \"instruction2\": {\n";
      os << "        \"function\": \"" << escapeJson(func2) << "\",\n";
      os << "        \"location\": \"" << escapeJson(loc2) << "\",\n";
      if (!src2.empty()) {
        os << "        \"source\": \"" << escapeJson(src2) << "\",\n";
      }
      if (tid2 != kUnknownThread) {
        os << "        \"thread_id\": " << tid2 << ",\n";
      }
      if (auto *load = dyn_cast<LoadInst>(inst2)) {
        std::string var = debugInfo.getVariableName(load->getPointerOperand());
        if (!var.empty()) {
          os << "        \"variable\": \"" << escapeJson(var) << "\",\n";
        }
      } else if (auto *store = dyn_cast<StoreInst>(inst2)) {
        std::string var = debugInfo.getVariableName(store->getPointerOperand());
        if (!var.empty()) {
          os << "        \"variable\": \"" << escapeJson(var) << "\",\n";
        }
      }
      os << "        \"type\": \""
         << (isa<LoadInst>(inst2)    ? "load"
             : isa<StoreInst>(inst2) ? "store"
                                     : "other")
         << "\"\n";
      os << "      }\n";
      os << "    }";
    }
  }
mhp_json_done:
  os << "\n  ],\n";

  // Lock Operations
  os << "  \"lock_operations\": [\n";
  size_t lockOpCount = 0;
  const size_t maxLockOpsToShow = 500;
  bool firstLock = true;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      if (m_threadAPI->isTDAcquire(inst) || m_threadAPI->isTDRelease(inst)) {
        if (lockOpCount++ >= maxLockOpsToShow)
          goto lockops_json_done;

        if (!firstLock)
          os << ",\n";
        firstLock = false;

        std::string funcName = debugInfo.getFunctionName(inst);
        std::string loc = debugInfo.getSourceLocation(inst);
        std::string src = debugInfo.getSourceCodeStatement(inst);
        std::string opType =
            m_threadAPI->isTDAcquire(inst) ? "acquire" : "release";

        os << "    {\n";
        os << "      \"operation\": \"" << opType << "\",\n";
        os << "      \"function\": \"" << escapeJson(funcName) << "\",\n";
        os << "      \"location\": \"" << escapeJson(loc) << "\",\n";
        if (!src.empty()) {
          os << "      \"source\": \"" << escapeJson(src) << "\",\n";
        }

        if (auto *call = dyn_cast<CallInst>(inst)) {
          if (call->arg_size() > 0) {
            const Value *lockArg = call->getArgOperand(0);
            std::string lockName = debugInfo.getVariableName(lockArg);
            if (!lockName.empty()) {
              os << "      \"lock\": \"" << escapeJson(lockName) << "\",\n";
            }
          }
        }

        LockSet mayLocks = m_locksetAnalysis->getMayLockSetAt(inst);
        if (!mayLocks.empty()) {
          os << "      \"may_hold_locks\": [";
          bool first = true;
          for (LockID lock : mayLocks) {
            if (!first)
              os << ", ";
            std::string lockName = debugInfo.getVariableName(lock);
            if (lockName.empty()) {
              lockName =
                  "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
            }
            os << "\"" << escapeJson(lockName) << "\"";
            first = false;
          }
          os << "],\n";
        }

        os << "      \"must_hold_locks\": [";
        LockSet mustLocks = m_locksetAnalysis->getMustLockSetAt(inst);
        bool firstMust = true;
        for (LockID lock : mustLocks) {
          if (!firstMust)
            os << ", ";
          std::string lockName = debugInfo.getVariableName(lock);
          if (lockName.empty()) {
            lockName =
                "lock@" + std::to_string(reinterpret_cast<uintptr_t>(lock));
          }
          os << "\"" << escapeJson(lockName) << "\"";
          firstMust = false;
        }
        os << "]\n";
        os << "    }";
      }
    }
  }
lockops_json_done:
  os << "\n  ],\n";

  // Escape Analysis
  os << "  \"escaped_variables\": [\n";
  std::set<const Value *> shownValues;
  bool firstEscaped = true;

  for (Function &func : m_module) {
    if (func.isDeclaration())
      continue;

    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;

      if (auto *load = dyn_cast<LoadInst>(inst)) {
        const Value *ptr = load->getPointerOperand();
        if (m_escapeAnalysis->isEscaped(ptr) &&
            shownValues.insert(ptr).second) {
          if (!firstEscaped)
            os << ",\n";
          firstEscaped = false;

          std::string funcName = debugInfo.getFunctionName(inst);
          std::string loc = debugInfo.getSourceLocation(inst);
          std::string varName = debugInfo.getVariableName(ptr);
          if (varName.empty()) {
            varName =
                "value@" + std::to_string(reinterpret_cast<uintptr_t>(ptr));
          }

          os << "    {\n";
          os << "      \"variable\": \"" << escapeJson(varName) << "\",\n";
          os << "      \"function\": \"" << escapeJson(funcName) << "\",\n";
          os << "      \"location\": \"" << escapeJson(loc) << "\"\n";
          os << "    }";
        }
      } else if (auto *store = dyn_cast<StoreInst>(inst)) {
        const Value *ptr = store->getPointerOperand();
        if (m_escapeAnalysis->isEscaped(ptr) &&
            shownValues.insert(ptr).second) {
          if (!firstEscaped)
            os << ",\n";
          firstEscaped = false;

          std::string funcName = debugInfo.getFunctionName(inst);
          std::string loc = debugInfo.getSourceLocation(inst);
          std::string varName = debugInfo.getVariableName(ptr);
          if (varName.empty()) {
            varName =
                "value@" + std::to_string(reinterpret_cast<uintptr_t>(ptr));
          }

          os << "    {\n";
          os << "      \"variable\": \"" << escapeJson(varName) << "\",\n";
          os << "      \"function\": \"" << escapeJson(funcName) << "\",\n";
          os << "      \"location\": \"" << escapeJson(loc) << "\"\n";
          os << "    }";
        }
      }
    }
  }
  os << "\n  ]\n";

  os << "}\n";
}

} // namespace concurrency

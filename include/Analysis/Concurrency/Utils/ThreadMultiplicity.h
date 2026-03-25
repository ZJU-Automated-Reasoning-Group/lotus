#pragma once

#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

namespace concurrency {

class ThreadMultiplicityAnalysis {
public:
  ThreadMultiplicityAnalysis(llvm::Module &module,
                             llvm::CallGraph *call_graph = nullptr)
      : m_module(module), m_call_graph(call_graph),
        m_thread_api(ThreadAPI::getThreadAPI()) {}

  bool instructionMayExecuteMultipleTimes(const llvm::Instruction *inst) const {
    if (!inst) {
      return true;
    }
    if (instructionIsInLoop(inst)) {
      return true;
    }
    return functionMayExecuteMultipleTimes(inst->getFunction());
  }

  bool functionMayExecuteMultipleTimes(const llvm::Function *func) const {
    if (!func || func->isDeclaration()) {
      return false;
    }

    auto memo_it = m_function_memo.find(func);
    if (memo_it != m_function_memo.end()) {
      return memo_it->second;
    }

    if (func->getName() == "main") {
      m_function_memo[func] = false;
      return false;
    }

    std::unordered_set<const llvm::Function *> visiting;
    const bool result = functionMayExecuteMultipleTimesImpl(func, visiting);
    m_function_memo[func] = result;
    return result;
  }

private:
  llvm::Module &m_module;
  llvm::CallGraph *m_call_graph;
  ThreadAPI *m_thread_api;
  mutable std::unordered_map<const llvm::Function *, bool> m_function_memo;
  mutable std::unordered_map<const llvm::Function *,
                             std::unique_ptr<llvm::DominatorTree>>
      m_dom_cache;

  bool functionMayExecuteMultipleTimesImpl(
      const llvm::Function *func,
      std::unordered_set<const llvm::Function *> &visiting) const {
    auto memo_it = m_function_memo.find(func);
    if (memo_it != m_function_memo.end()) {
      return memo_it->second;
    }

    if (!visiting.insert(func).second) {
      return true;
    }

    const std::set<const llvm::Instruction *> call_sites = collectCallSites(func);
    if (call_sites.size() > 1) {
      visiting.erase(func);
      m_function_memo[func] = true;
      return true;
    }

    for (const llvm::Instruction *call_site : call_sites) {
      if (instructionIsInLoop(call_site)) {
        visiting.erase(func);
        m_function_memo[func] = true;
        return true;
      }

      const llvm::Function *caller = call_site ? call_site->getFunction() : nullptr;
      if (!caller || caller == func) {
        visiting.erase(func);
        m_function_memo[func] = true;
        return true;
      }

      if (functionMayExecuteMultipleTimesImpl(caller, visiting)) {
        visiting.erase(func);
        m_function_memo[func] = true;
        return true;
      }
    }

    visiting.erase(func);
    m_function_memo[func] = false;
    return false;
  }

  std::set<const llvm::Instruction *>
  collectCallSites(const llvm::Function *target) const {
    std::set<const llvm::Instruction *> call_sites;
    if (!target) {
      return call_sites;
    }

    for (const llvm::User *user : target->users()) {
      const auto *cb = llvm::dyn_cast<llvm::CallBase>(user);
      if (!cb) {
        continue;
      }
      if (cb->getCalledFunction() == target ||
          cb->getCalledOperand()->stripPointerCasts() == target) {
        call_sites.insert(cb);
      }
    }

    for (const llvm::Function &func : m_module) {
      if (func.isDeclaration()) {
        continue;
      }
      for (const llvm::Instruction &inst : llvm::instructions(func)) {
        const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst);
        if (!cb) {
          continue;
        }
        if (m_thread_api && m_thread_api->isTDFork(cb) &&
            m_thread_api->getForkedFun(cb) == target) {
          call_sites.insert(&inst);
        }
        const llvm::Value *called = cb->getCalledOperand();
        if (called && called->stripPointerCasts() == target) {
          call_sites.insert(&inst);
        }
      }
    }

    if (!m_call_graph) {
      return call_sites;
    }

    for (const llvm::Function &func : m_module) {
      if (func.isDeclaration()) {
        continue;
      }
      llvm::CallGraphNode *node = (*m_call_graph)[const_cast<llvm::Function *>(&func)];
      if (!node) {
        continue;
      }
      for (auto &record : *node) {
        if (!record.first.hasValue()) {
          continue;
        }
        llvm::CallGraphNode *callee_node = record.second;
        if (!callee_node || callee_node->getFunction() != target) {
          continue;
        }
        if (const auto *cb =
                llvm::dyn_cast_or_null<llvm::CallBase>(*record.first)) {
          call_sites.insert(cb);
        }
      }
    }

    return call_sites;
  }

  bool instructionIsInLoop(const llvm::Instruction *inst) const {
    if (!inst || !inst->getFunction()) {
      return true;
    }
    const llvm::Function *func = inst->getFunction();
    llvm::LoopInfo loop_info;
    loop_info.analyze(getDomTree(func));
    return loop_info.getLoopFor(inst->getParent()) != nullptr;
  }

  llvm::DominatorTree &getDomTree(const llvm::Function *func) const {
    auto it = m_dom_cache.find(func);
    if (it != m_dom_cache.end()) {
      return *it->second;
    }
    auto dt = std::make_unique<llvm::DominatorTree>();
    dt->recalculate(*const_cast<llvm::Function *>(func));
    llvm::DominatorTree *dt_ptr = dt.get();
    m_dom_cache[func] = std::move(dt);
    return *dt_ptr;
  }
};

} // namespace concurrency

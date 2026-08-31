#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/LockSet/LockSetAnalysis.h"
#include "Concurrency/LockSet/LockSetAnalysisSupport.h"

#include <functional>
#include <limits>
#include <queue>
#include <set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace mhp;

void LockSetAnalysis::analyzeFunction(Function *func) {
  if (!func || func->isDeclaration())
    return;

  RAIILock::RAIILockTracker raii_tracker;
  raii_tracker.analyzeFunction(func);
  m_raii_locks[func] = raii_tracker.getAllLockLifetimes();

  detectTryLockSuccessBranches(func);
  computeIntraproceduralLockSets(func);
}

void LockSetAnalysis::detectTryLockSuccessBranches(Function *func) {
  if (!func || func->isDeclaration())
    return;

  for (auto it = m_trylock_edge_refinements.begin();
       it != m_trylock_edge_refinements.end();) {
    const Instruction *source = it->first.first;
    if (source && source->getFunction() == func) {
      it = m_trylock_edge_refinements.erase(it);
    } else {
      ++it;
    }
  }

  struct BooleanTryCondition {
    const CallBase *call = nullptr;
    bool true_means_nonzero = true;
  };
  auto traceBooleanTryCondition = [&](const Value *root) {
    std::function<BooleanTryCondition(const Value *, bool,
                                      std::unordered_set<const Value *> &)>
        trace;
    trace = [&](const Value *value, bool truth,
                std::unordered_set<const Value *> &visited)
        -> BooleanTryCondition {
      if (!value || !visited.insert(value).second)
        return {};
      if (const auto *call = dyn_cast<CallBase>(value))
        return m_thread_api->isConditionalLockAcquire(call)
                   ? BooleanTryCondition{call, truth}
                   : BooleanTryCondition{};
      if (const auto *freeze = dyn_cast<FreezeInst>(value))
        return trace(freeze->getOperand(0), truth, visited);
      if (const auto *cast = dyn_cast<CastInst>(value)) {
        if (isa<ZExtInst>(cast) || isa<SExtInst>(cast)) {
          return trace(cast->getOperand(0), truth, visited);
        }
        return {};
      }
      if (const auto *binary = dyn_cast<BinaryOperator>(value)) {
        if (binary->getOpcode() == Instruction::Xor) {
          const ConstantInt *constant = dyn_cast<ConstantInt>(binary->getOperand(1));
          const Value *forwarded = binary->getOperand(0);
          if (!constant) {
            constant = dyn_cast<ConstantInt>(binary->getOperand(0));
            forwarded = binary->getOperand(1);
          }
          if (binary->getType()->isIntegerTy(1) && constant &&
              constant->isOne())
            return trace(forwarded, !truth, visited);
        }
      }
      if (const auto *phi = dyn_cast<PHINode>(value)) {
        BooleanTryCondition result;
        for (const Value *incoming : phi->incoming_values()) {
          std::unordered_set<const Value *> branch_visited = visited;
          BooleanTryCondition candidate = trace(incoming, truth, branch_visited);
          if (!candidate.call)
            return {};
          if (!result.call)
            result = candidate;
          else if (result.call != candidate.call ||
                   result.true_means_nonzero != candidate.true_means_nonzero)
            return {};
        }
        return result;
      }
      if (const auto *select = dyn_cast<SelectInst>(value)) {
        const auto *true_const = dyn_cast<ConstantInt>(select->getTrueValue());
        const auto *false_const = dyn_cast<ConstantInt>(select->getFalseValue());
        if (true_const && false_const && true_const->getType()->isIntegerTy(1) &&
            false_const->getType()->isIntegerTy(1) &&
            true_const->getValue() != false_const->getValue()) {
          const bool condition_truth = truth == true_const->isOne();
          return trace(select->getCondition(), condition_truth, visited);
        }
      }
      return {};
    };
    std::unordered_set<const Value *> visited;
    return trace(root, true, visited);
  };

  for (BasicBlock &bb : *func) {
    auto *br = dyn_cast<BranchInst>(bb.getTerminator());
    if (!br || !br->isConditional())
      continue;

    const CallBase *trylock_call = nullptr;
    bool true_branch_is_nonzero = true;
    if (const auto *cmp = dyn_cast<ICmpInst>(br->getCondition())) {
      if (cmp->getPredicate() != ICmpInst::ICMP_EQ &&
          cmp->getPredicate() != ICmpInst::ICMP_NE)
        continue;
      const ConstantInt *compared_const =
          dyn_cast<ConstantInt>(cmp->getOperand(1));
      const Value *trylock_ret = cmp->getOperand(0);
      if (!compared_const) {
        compared_const = dyn_cast<ConstantInt>(cmp->getOperand(0));
        trylock_ret = cmp->getOperand(1);
      }
      if (!compared_const ||
          (!compared_const->isZero() &&
           !(compared_const->isOne() &&
             trylock_ret->getType()->isIntegerTy(1))))
        continue;
      BooleanTryCondition traced = traceBooleanTryCondition(trylock_ret);
      trylock_call = traced.call;
      const bool comparison_true_means_nonzero =
          (cmp->getPredicate() == ICmpInst::ICMP_NE) ==
          compared_const->isZero();
      true_branch_is_nonzero = traced.true_means_nonzero ==
                               comparison_true_means_nonzero;
    } else {
      BooleanTryCondition traced =
          traceBooleanTryCondition(br->getCondition());
      trylock_call = traced.call;
      true_branch_is_nonzero = traced.true_means_nonzero;
    }
    if (!trylock_call)
      continue;

    const ThreadAPI::LockSemanticInfo semantics =
        m_thread_api->getLockSemanticInfo(trylock_call);
    LockID lock = getLockValue(trylock_call);
    if (!lock || semantics.try_success == ThreadAPI::TryLockSuccess::Unknown)
      continue;

    const bool zero_means_success =
        semantics.try_success == ThreadAPI::TryLockSuccess::Zero;

    bool true_branch_is_success =
        zero_means_success ? !true_branch_is_nonzero : true_branch_is_nonzero;

    const unsigned success_index = true_branch_is_success ? 0 : 1;
    const unsigned failure_index = true_branch_is_success ? 1 : 0;
    m_trylock_edge_refinements[{br, success_index}].push_back(
        {trylock_call, lock, semantics.kind, true});
    m_trylock_edge_refinements[{br, failure_index}].push_back(
        {trylock_call, lock, semantics.kind, false});
  }

  for (BasicBlock &bb : *func) {
    const auto *switch_inst = dyn_cast<SwitchInst>(bb.getTerminator());
    if (!switch_inst || switch_inst->getNumCases() != 1)
      continue;
    const auto switch_case = *switch_inst->case_begin();
    if (!switch_case.getCaseValue()->isZero())
      continue;
    BooleanTryCondition traced =
        traceBooleanTryCondition(switch_inst->getCondition());
    if (!traced.call)
      continue;
    const ThreadAPI::LockSemanticInfo semantics =
        m_thread_api->getLockSemanticInfo(traced.call);
    LockID lock = getLockValue(traced.call);
    if (!lock || semantics.try_success == ThreadAPI::TryLockSuccess::Unknown)
      continue;
    const bool zero_means_success =
        semantics.try_success == ThreadAPI::TryLockSuccess::Zero;
    const bool condition_zero_means_success =
        zero_means_success == traced.true_means_nonzero;
    const unsigned success_index = condition_zero_means_success ? 1 : 0;
    const unsigned failure_index = condition_zero_means_success ? 0 : 1;
    m_trylock_edge_refinements[{switch_inst, success_index}].push_back(
        {traced.call, lock, semantics.kind, true});
    m_trylock_edge_refinements[{switch_inst, failure_index}].push_back(
        {traced.call, lock, semantics.kind, false});
  }
}

void LockSetAnalysis::computeIntraproceduralLockSets(Function *func) {
  auto clearFunctionFacts = [&](Function *target) {
    if (!target) {
      return;
    }
    for (Instruction &inst : instructions(target)) {
      const Instruction *key = &inst;
      m_may_locksets_entry.erase(key);
      m_may_locksets_exit.erase(key);
      m_must_locksets_entry.erase(key);
      m_must_locksets_exit.erase(key);
      m_may_read_locks_entry.erase(key);
      m_may_read_locks_exit.erase(key);
      m_may_write_locks_entry.erase(key);
      m_may_write_locks_exit.erase(key);
      m_must_read_locks_entry.erase(key);
      m_must_read_locks_exit.erase(key);
      m_must_write_locks_entry.erase(key);
      m_must_write_locks_exit.erase(key);
      m_may_recursive_depth_entry.erase(key);
      m_may_recursive_depth_exit.erase(key);
      m_must_recursive_depth_entry.erase(key);
      m_must_recursive_depth_exit.erase(key);
      m_may_raii_ownership_entry.erase(key);
      m_may_raii_ownership_exit.erase(key);
      m_must_raii_ownership_entry.erase(key);
      m_must_raii_ownership_exit.erase(key);
      m_invoke_normal_exit.erase(key);
      m_invoke_unwind_exit.erase(key);
    }
  };

  clearFunctionFacts(func);

  auto applyTryLockEdgeRefinement =
      [&](const BasicBlock *pred, const BasicBlock *succ, LockSet &may,
          LockSet &must, LockSet &may_read, LockSet &may_write,
          LockSet &must_read, LockSet &must_write) {
        const Instruction *term = pred ? pred->getTerminator() : nullptr;
        unsigned matching_index = 0;
        unsigned matches = 0;
        if (term) {
          for (unsigned index = 0; index < term->getNumSuccessors(); ++index) {
            if (term->getSuccessor(index) == succ) {
              matching_index = index;
              ++matches;
            }
          }
        }
        if (matches != 1) {
          return;
        }
        auto refinement_it =
            m_trylock_edge_refinements.find({term, matching_index});
        if (refinement_it == m_trylock_edge_refinements.end()) {
          return;
        }

        auto containedBeforeTry = [](const auto &facts, const CallBase *call,
                                     LockID lock) {
          auto fact_it = facts.find(call);
          return fact_it != facts.end() && fact_it->second.count(lock) != 0;
        };

        for (const TryLockEdgeRefinement &refinement : refinement_it->second) {
          if (refinement.success) {
            may.insert(refinement.lock);
            must.insert(refinement.lock);
            if (refinement.mode == ThreadAPI::LockSemanticKind::Shared) {
              may_read.insert(refinement.lock);
              must_read.insert(refinement.lock);
            } else {
              may_write.insert(refinement.lock);
              must_write.insert(refinement.lock);
            }
            continue;
          }

          if (!containedBeforeTry(m_may_locksets_entry, refinement.call,
                                  refinement.lock)) {
            may.erase(refinement.lock);
          }
          if (!containedBeforeTry(m_must_locksets_entry, refinement.call,
                                  refinement.lock)) {
            must.erase(refinement.lock);
          }
          if (refinement.mode == ThreadAPI::LockSemanticKind::Shared) {
            if (!containedBeforeTry(m_may_read_locks_entry, refinement.call,
                                    refinement.lock)) {
              may_read.erase(refinement.lock);
            }
            if (!containedBeforeTry(m_must_read_locks_entry, refinement.call,
                                    refinement.lock)) {
              must_read.erase(refinement.lock);
            }
          } else {
            if (!containedBeforeTry(m_may_write_locks_entry, refinement.call,
                                    refinement.lock)) {
              may_write.erase(refinement.lock);
            }
            if (!containedBeforeTry(m_must_write_locks_entry, refinement.call,
                                    refinement.lock)) {
              must_write.erase(refinement.lock);
            }
          }
        }
      };

  std::queue<const Instruction *> worklist;
  std::set<const Instruction *> in_worklist;

  unsigned recursive_acquire_limit = 0;
  auto countRecursiveAcquires = [&](const Function &candidate_func) {
    for (const Instruction &candidate : instructions(candidate_func)) {
      const auto *candidate_call = dyn_cast<CallBase>(&candidate);
      const Function *candidate_callee =
          candidate_call ? m_thread_api->getCallee(candidate_call) : nullptr;
      if (candidate_callee &&
          candidate_callee->getName().contains("recursive_mutex") &&
          m_thread_api->isTDAcquire(&candidate) &&
          !m_thread_api->isTryLock(&candidate)) {
        ++recursive_acquire_limit;
      }
    }
  };
  if (m_module) {
    for (const Function &candidate_func : *m_module) {
      if (!candidate_func.isDeclaration()) {
        countRecursiveAcquires(candidate_func);
      }
    }
  } else {
    countRecursiveAcquires(*func);
  }
  recursive_acquire_limit = std::max(1u, recursive_acquire_limit);

  const Instruction *entry = &func->getEntryBlock().front();
  worklist.push(entry);
  in_worklist.insert(entry);

  while (!worklist.empty()) {
    const Instruction *inst = worklist.front();
    worklist.pop();
    in_worklist.erase(inst);

    std::vector<LockSet> may_inputs, must_inputs;
    std::vector<LockSet> may_read_inputs, may_write_inputs;
    std::vector<LockSet> must_read_inputs, must_write_inputs;
    std::vector<RecursiveDepthMap> may_depth_inputs, must_depth_inputs;
    std::vector<RAIIWrapperSet> may_ownership_inputs, must_ownership_inputs;

    if (inst == entry) {
      may_inputs.push_back(LockSet());
      must_inputs.push_back(LockSet());
      may_read_inputs.push_back(LockSet());
      may_write_inputs.push_back(LockSet());
      must_read_inputs.push_back(LockSet());
      must_write_inputs.push_back(LockSet());
      may_depth_inputs.emplace_back();
      must_depth_inputs.emplace_back();
      may_ownership_inputs.emplace_back();
      must_ownership_inputs.emplace_back();
    } else {
      const BasicBlock *bb = inst->getParent();
      if (inst == &bb->front()) {
        for (const BasicBlock *pred : predecessors(bb)) {
          const Instruction *pred_term = pred->getTerminator();
          if (!pred_term) {
            continue;
          }

          auto it_may = m_may_locksets_exit.find(pred_term);
          auto it_must = m_must_locksets_exit.find(pred_term);
          auto it_mr = m_may_read_locks_exit.find(pred_term);
          auto it_mw = m_may_write_locks_exit.find(pred_term);
          auto it_ur = m_must_read_locks_exit.find(pred_term);
          auto it_uw = m_must_write_locks_exit.find(pred_term);

          // Unreached predecessors represent lattice TOP for MUST. Ignore
          // them until they have a concrete dataflow value.
          if (it_may == m_may_locksets_exit.end()) {
            continue;
          }

          LockSet pred_may = it_may->second;
          LockSet pred_must = it_must != m_must_locksets_exit.end()
                                  ? it_must->second
                                  : LockSet();
          LockSet pred_may_read =
              it_mr != m_may_read_locks_exit.end() ? it_mr->second : LockSet();
          LockSet pred_may_write =
              it_mw != m_may_write_locks_exit.end() ? it_mw->second : LockSet();
          LockSet pred_must_read =
              it_ur != m_must_read_locks_exit.end() ? it_ur->second : LockSet();
          LockSet pred_must_write = it_uw != m_must_write_locks_exit.end()
                                        ? it_uw->second
                                        : LockSet();

          if (const auto *invoke = dyn_cast<InvokeInst>(pred_term)) {
            const auto &edge_facts = invoke->getNormalDest() == bb
                                         ? m_invoke_normal_exit
                                         : m_invoke_unwind_exit;
            auto edge_it = edge_facts.find(pred_term);
            if (edge_it == edge_facts.end()) {
              continue;
            }
            pred_may = edge_it->second.may_lockset;
            pred_must = edge_it->second.must_lockset;
            pred_may_read = edge_it->second.may_read_lockset;
            pred_may_write = edge_it->second.may_write_lockset;
            pred_must_read = edge_it->second.must_read_lockset;
            pred_must_write = edge_it->second.must_write_lockset;
            may_depth_inputs.push_back(edge_it->second.may_recursive_depth);
            must_depth_inputs.push_back(edge_it->second.must_recursive_depth);
          }

          applyTryLockEdgeRefinement(pred, bb, pred_may, pred_must,
                                     pred_may_read, pred_may_write,
                                     pred_must_read, pred_must_write);
          may_inputs.push_back(std::move(pred_may));
          must_inputs.push_back(std::move(pred_must));
          may_read_inputs.push_back(std::move(pred_may_read));
          may_write_inputs.push_back(std::move(pred_may_write));
          must_read_inputs.push_back(std::move(pred_must_read));
          must_write_inputs.push_back(std::move(pred_must_write));
          if (!isa<InvokeInst>(pred_term)) {
            auto may_depth_it = m_may_recursive_depth_exit.find(pred_term);
            auto must_depth_it = m_must_recursive_depth_exit.find(pred_term);
            may_depth_inputs.push_back(
                may_depth_it != m_may_recursive_depth_exit.end()
                    ? may_depth_it->second
                    : RecursiveDepthMap());
            must_depth_inputs.push_back(
                must_depth_it != m_must_recursive_depth_exit.end()
                    ? must_depth_it->second
                    : RecursiveDepthMap());
          }
          auto may_ownership_it = m_may_raii_ownership_exit.find(pred_term);
          auto must_ownership_it = m_must_raii_ownership_exit.find(pred_term);
          may_ownership_inputs.push_back(
              may_ownership_it != m_may_raii_ownership_exit.end()
                  ? may_ownership_it->second
                  : RAIIWrapperSet());
          must_ownership_inputs.push_back(
              must_ownership_it != m_must_raii_ownership_exit.end()
                  ? must_ownership_it->second
                  : RAIIWrapperSet());
        }
      } else if (const Instruction *prev = inst->getPrevNode()) {
        auto it_may = m_may_locksets_exit.find(prev);
        if (it_may != m_may_locksets_exit.end()) {
          may_inputs.push_back(it_may->second);
          auto it_must = m_must_locksets_exit.find(prev);
          must_inputs.push_back(it_must != m_must_locksets_exit.end()
                                    ? it_must->second
                                    : LockSet());
          auto it_mr = m_may_read_locks_exit.find(prev);
          may_read_inputs.push_back(
              it_mr != m_may_read_locks_exit.end() ? it_mr->second : LockSet());
          auto it_mw = m_may_write_locks_exit.find(prev);
          may_write_inputs.push_back(it_mw != m_may_write_locks_exit.end()
                                         ? it_mw->second
                                         : LockSet());
          auto it_ur = m_must_read_locks_exit.find(prev);
          must_read_inputs.push_back(it_ur != m_must_read_locks_exit.end()
                                         ? it_ur->second
                                         : LockSet());
          auto it_uw = m_must_write_locks_exit.find(prev);
          must_write_inputs.push_back(it_uw != m_must_write_locks_exit.end()
                                          ? it_uw->second
                                          : LockSet());
          auto may_depth_it = m_may_recursive_depth_exit.find(prev);
          auto must_depth_it = m_must_recursive_depth_exit.find(prev);
          may_depth_inputs.push_back(may_depth_it !=
                                             m_may_recursive_depth_exit.end()
                                         ? may_depth_it->second
                                         : RecursiveDepthMap());
          must_depth_inputs.push_back(must_depth_it !=
                                              m_must_recursive_depth_exit.end()
                                          ? must_depth_it->second
                                          : RecursiveDepthMap());
          auto may_ownership_it = m_may_raii_ownership_exit.find(prev);
          auto must_ownership_it = m_must_raii_ownership_exit.find(prev);
          may_ownership_inputs.push_back(
              may_ownership_it != m_may_raii_ownership_exit.end()
                  ? may_ownership_it->second
                  : RAIIWrapperSet());
          must_ownership_inputs.push_back(
              must_ownership_it != m_must_raii_ownership_exit.end()
                  ? must_ownership_it->second
                  : RAIIWrapperSet());
        }
      }
    }

    auto mergeDepths = [](const std::vector<RecursiveDepthMap> &inputs,
                          bool is_must) {
      RecursiveDepthMap result;
      if (inputs.empty()) {
        return result;
      }
      if (!is_must) {
        for (const RecursiveDepthMap &input : inputs) {
          for (const auto &[lock, depth] : input) {
            result[lock] = std::max(result[lock], depth);
          }
        }
        return result;
      }

      result = inputs.front();
      for (size_t index = 1; index < inputs.size(); ++index) {
        for (auto it = result.begin(); it != result.end();) {
          auto other = inputs[index].find(it->first);
          if (other == inputs[index].end()) {
            it = result.erase(it);
          } else {
            it->second = std::min(it->second, other->second);
            ++it;
          }
        }
      }
      return result;
    };

    RecursiveDepthMap may_depth_in = mergeDepths(may_depth_inputs, false);
    RecursiveDepthMap must_depth_in = mergeDepths(must_depth_inputs, true);
    auto mergeOwnership = [](const std::vector<RAIIWrapperSet> &inputs,
                             bool is_must) {
      RAIIWrapperSet result;
      if (inputs.empty()) {
        return result;
      }
      if (!is_must) {
        for (const RAIIWrapperSet &input : inputs) {
          result.insert(input.begin(), input.end());
        }
        return result;
      }
      result = inputs.front();
      for (size_t index = 1; index < inputs.size(); ++index) {
        RAIIWrapperSet intersection;
        std::set_intersection(result.begin(), result.end(),
                              inputs[index].begin(), inputs[index].end(),
                              std::inserter(intersection,
                                            intersection.begin()));
        result = std::move(intersection);
      }
      return result;
    };
    RAIIWrapperSet may_ownership_in =
        mergeOwnership(may_ownership_inputs, false);
    RAIIWrapperSet must_ownership_in =
        mergeOwnership(must_ownership_inputs, true);
    const bool may_ownership_entry_changed =
        m_may_raii_ownership_entry[inst] != may_ownership_in;
    const bool must_ownership_entry_changed =
        m_must_raii_ownership_entry[inst] != must_ownership_in;
    m_may_raii_ownership_entry[inst] = may_ownership_in;
    m_must_raii_ownership_entry[inst] = must_ownership_in;
    RAIIWrapperSet may_ownership_out =
        transferRAIIOwnership(inst, may_ownership_in, false);
    RAIIWrapperSet must_ownership_out =
        transferRAIIOwnership(inst, must_ownership_in, true);

    LockSet may_read_in =
        may_read_inputs.empty() ? LockSet() : merge(may_read_inputs, false);
    LockSet may_write_in =
        may_write_inputs.empty() ? LockSet() : merge(may_write_inputs, false);
    LockSet must_read_in =
        must_read_inputs.empty() ? LockSet() : merge(must_read_inputs, true);
    LockSet must_write_in =
        must_write_inputs.empty() ? LockSet() : merge(must_write_inputs, true);
    LockSet may_in = may_read_in;
    may_in.insert(may_write_in.begin(), may_write_in.end());
    LockSet must_in = must_read_in;
    must_in.insert(must_write_in.begin(), must_write_in.end());

    LockSet may_read_out, may_write_out, must_read_out, must_write_out;
    transferReadWrite(inst, may_read_in, may_write_in, may_read_out,
                      may_write_out, false);
    transferReadWrite(inst, must_read_in, must_write_in, must_read_out,
                      must_write_out, true);

    LockSet may_out = transfer(inst, may_in, false);
    LockSet must_out = transfer(inst, must_in, true);

    RecursiveDepthMap may_depth_out = may_depth_in;
    RecursiveDepthMap must_depth_out = must_depth_in;
    const CallBase *depth_call = dyn_cast<CallBase>(inst);
    const Function *depth_callee =
        depth_call ? m_thread_api->getCallee(depth_call) : nullptr;
    const bool depth_lock_operation =
        depth_call && (m_thread_api->isTDAcquire(inst) ||
                       m_thread_api->isTDRelease(inst));
    LockID depth_lock =
        depth_lock_operation ? getLockValue(depth_call) : nullptr;
    const bool known_recursive_mutex =
        depth_callee && depth_lock &&
        depth_callee->getName().contains("recursive_mutex");
    constexpr unsigned UNBOUNDED_RECURSIVE_DEPTH =
        std::numeric_limits<unsigned>::max();
    if (known_recursive_mutex && !m_thread_api->isTryLock(inst)) {
      depth_lock = getCanonicalLock(depth_lock);
      if (m_thread_api->isTDAcquire(inst)) {
        unsigned &may_depth = may_depth_out[depth_lock];
        if (may_depth == UNBOUNDED_RECURSIVE_DEPTH ||
            may_depth >= recursive_acquire_limit) {
          may_depth = UNBOUNDED_RECURSIVE_DEPTH;
        } else {
          ++may_depth;
        }
        ++must_depth_out[depth_lock];
      } else if (m_thread_api->isTDRelease(inst)) {
        auto applyReleaseDepth = [&](RecursiveDepthMap &depths, LockSet &locks,
                                     LockSet &write_locks) {
          auto depth_it = depths.find(depth_lock);
          if (depth_it == depths.end()) {
            return;
          }
          if (depth_it->second > 1) {
            if (depth_it->second != UNBOUNDED_RECURSIVE_DEPTH) {
              --depth_it->second;
            }
            locks.insert(depth_lock);
            write_locks.insert(depth_lock);
          } else {
            depths.erase(depth_it);
          }
        };
        applyReleaseDepth(may_depth_out, may_out, may_write_out);
        applyReleaseDepth(must_depth_out, must_out, must_write_out);
      }
    }
    if (depth_call && !m_thread_api->isTDAcquire(inst) &&
        !m_thread_api->isTDRelease(inst)) {
      for (Function *callee : getCallees(depth_call)) {
        auto summary_it = m_function_summaries.find(callee);
        if (summary_it == m_function_summaries.end() ||
            !summary_it->second.is_analyzed) {
          continue;
        }
        const FunctionSummary &summary = summary_it->second;
        for (const auto &[summary_lock, delta] :
             summary.may_recursive_acquire_delta) {
          if (LockID instantiated =
                  instantiateSummaryLock(depth_call, callee, summary_lock)) {
            unsigned &depth = may_depth_out[instantiated];
            if (depth == UNBOUNDED_RECURSIVE_DEPTH ||
                delta == UNBOUNDED_RECURSIVE_DEPTH ||
                depth > UNBOUNDED_RECURSIVE_DEPTH - delta ||
                depth + delta > recursive_acquire_limit) {
              depth = UNBOUNDED_RECURSIVE_DEPTH;
            } else {
              depth += delta;
            }
          }
        }
        for (const auto &[summary_lock, delta] :
             summary.must_recursive_acquire_delta) {
          if (LockID instantiated =
                  instantiateSummaryLock(depth_call, callee, summary_lock)) {
            unsigned &depth = must_depth_out[instantiated];
            if (depth == UNBOUNDED_RECURSIVE_DEPTH ||
                delta == UNBOUNDED_RECURSIVE_DEPTH ||
                depth > UNBOUNDED_RECURSIVE_DEPTH - delta ||
                depth + delta > recursive_acquire_limit) {
              depth = UNBOUNDED_RECURSIVE_DEPTH;
            } else {
              depth += delta;
            }
          }
        }
        auto applyRecursiveRelease =
            [&](RecursiveDepthMap &depths, LockSet &locks,
                LockSet &write_locks, const RecursiveDepthMap &effects) {
              for (const auto &[summary_lock, delta] : effects) {
                LockID lock =
                    instantiateSummaryLock(depth_call, callee, summary_lock);
                auto depth_it = depths.find(lock);
                if (!lock || depth_it == depths.end()) {
                  continue;
                }
                if (depth_it->second == UNBOUNDED_RECURSIVE_DEPTH ||
                    depth_it->second > delta) {
                  if (depth_it->second != UNBOUNDED_RECURSIVE_DEPTH) {
                    depth_it->second -= delta;
                  }
                  locks.insert(lock);
                  write_locks.insert(lock);
                } else {
                  depths.erase(depth_it);
                }
              }
            };
        applyRecursiveRelease(may_depth_out, may_out, may_write_out,
                              summary.must_recursive_release_delta);
        applyRecursiveRelease(must_depth_out, must_out, must_write_out,
                              summary.may_recursive_release_delta);
      }
    }

    if (const auto *invoke = dyn_cast<InvokeInst>(inst)) {
      if (!invoke->doesNotThrow()) {
        InvokeEdgeFacts normal_facts;
        normal_facts.may_lockset = may_out;
        normal_facts.must_lockset = must_out;
        normal_facts.may_read_lockset = may_read_out;
        normal_facts.may_write_lockset = may_write_out;
        normal_facts.must_read_lockset = must_read_out;
        normal_facts.must_write_lockset = must_write_out;
        normal_facts.may_recursive_depth = may_depth_out;
        normal_facts.must_recursive_depth = must_depth_out;
        m_invoke_normal_exit[inst] = normal_facts;

        std::vector<LockSet> unwind_may_results;
        std::vector<LockSet> unwind_must_results;
        std::vector<LockSet> unwind_may_read_results;
        std::vector<LockSet> unwind_may_write_results;
        std::vector<LockSet> unwind_must_read_results;
        std::vector<LockSet> unwind_must_write_results;
        std::vector<RecursiveDepthMap> unwind_may_depth_results;
        std::vector<RecursiveDepthMap> unwind_must_depth_results;

        for (Function *callee : getCallees(invoke)) {
          auto summary_it = m_function_summaries.find(callee);
          if (summary_it == m_function_summaries.end() ||
              !summary_it->second.is_analyzed ||
              !summary_it->second.has_exceptional_exit) {
            continue;
          }

          LockSet candidate_may = may_in;
          LockSet candidate_must = must_in;
          RecursiveDepthMap candidate_may_depth = may_depth_in;
          RecursiveDepthMap candidate_must_depth = must_depth_in;
          if (!applyExceptionalFunctionSummary(invoke, callee, candidate_may,
                                               candidate_must)) {
            continue;
          }

          LockSet candidate_may_read = may_read_in;
          LockSet candidate_may_write = may_write_in;
          LockSet candidate_must_read = must_read_in;
          LockSet candidate_must_write = must_write_in;
          const FunctionSummary &summary = summary_it->second;
          for (const auto &[lock, delta] :
               summary.exceptional_may_recursive_acquire_delta) {
            if (LockID instantiated =
                    instantiateSummaryLock(invoke, callee, lock)) {
              candidate_may_depth[instantiated] += delta;
            }
          }
          for (const auto &[lock, delta] :
               summary.exceptional_must_recursive_acquire_delta) {
            if (LockID instantiated =
                    instantiateSummaryLock(invoke, callee, lock)) {
              candidate_must_depth[instantiated] += delta;
            }
          }
          auto applyExceptionalRecursiveRelease =
              [&](RecursiveDepthMap &depths, LockSet &locks,
                  LockSet &write_locks, const RecursiveDepthMap &effects) {
                for (const auto &[lock, delta] : effects) {
                  LockID instantiated =
                      instantiateSummaryLock(invoke, callee, lock);
                  auto depth_it = depths.find(instantiated);
                  if (!instantiated || depth_it == depths.end()) {
                    continue;
                  }
                  if (depth_it->second ==
                          std::numeric_limits<unsigned>::max() ||
                      depth_it->second > delta) {
                    if (depth_it->second !=
                        std::numeric_limits<unsigned>::max()) {
                      depth_it->second -= delta;
                    }
                    locks.insert(instantiated);
                    write_locks.insert(instantiated);
                  } else {
                    depths.erase(depth_it);
                  }
                }
              };
          applyExceptionalRecursiveRelease(
              candidate_may_depth, candidate_may, candidate_may_write,
              summary.exceptional_must_recursive_release_delta);
          applyExceptionalRecursiveRelease(
              candidate_must_depth, candidate_must, candidate_must_write,
              summary.exceptional_may_recursive_release_delta);

          auto eraseMustReleased = [&](LockID lock, LockSet &read,
                                       LockSet &write) {
            if (!lock) {
              return;
            }
            read.erase(lock);
            write.erase(lock);
            LockSet remove_read;
            LockSet remove_write;
            for (LockID held : read) {
              if (mayAlias(held, lock)) {
                remove_read.insert(held);
              }
            }
            for (LockID held : write) {
              if (mayAlias(held, lock)) {
                remove_write.insert(held);
              }
            }
            for (LockID held : remove_read) {
              read.erase(held);
            }
            for (LockID held : remove_write) {
              write.erase(held);
            }
          };

          for (LockID lock : summary.exceptional_may_read_acquire_delta) {
            if (LockID instantiated =
                    instantiateSummaryLock(invoke, callee, lock)) {
              candidate_may_read.insert(instantiated);
            }
          }
          for (LockID lock : summary.exceptional_may_write_acquire_delta) {
            if (LockID instantiated =
                    instantiateSummaryLock(invoke, callee, lock)) {
              candidate_may_write.insert(instantiated);
            }
          }
          for (LockID lock : summary.exceptional_must_read_acquire_delta) {
            if (LockID instantiated =
                    instantiateSummaryLock(invoke, callee, lock)) {
              candidate_must_read.insert(instantiated);
            }
          }
          for (LockID lock : summary.exceptional_must_write_acquire_delta) {
            if (LockID instantiated =
                    instantiateSummaryLock(invoke, callee, lock)) {
              candidate_must_write.insert(instantiated);
            }
          }
          for (LockID lock : summary.exceptional_may_release_delta) {
            eraseMustReleased(instantiateSummaryLock(invoke, callee, lock),
                              candidate_must_read, candidate_must_write);
          }
          for (LockID lock : summary.exceptional_must_release_delta) {
            LockID instantiated = instantiateSummaryLock(invoke, callee, lock);
            candidate_may_read.erase(instantiated);
            candidate_may_write.erase(instantiated);
            eraseMustReleased(instantiated, candidate_must_read,
                              candidate_must_write);
          }

          unwind_may_results.push_back(std::move(candidate_may));
          unwind_must_results.push_back(std::move(candidate_must));
          unwind_may_read_results.push_back(std::move(candidate_may_read));
          unwind_may_write_results.push_back(std::move(candidate_may_write));
          unwind_must_read_results.push_back(std::move(candidate_must_read));
          unwind_must_write_results.push_back(std::move(candidate_must_write));
          unwind_may_depth_results.push_back(std::move(candidate_may_depth));
          unwind_must_depth_results.push_back(std::move(candidate_must_depth));
        }

        if (hasUnresolvedCalleeTarget(invoke)) {
          unwind_may_results.push_back(may_in);
          unwind_must_results.emplace_back();
          unwind_may_read_results.push_back(may_read_in);
          unwind_may_write_results.push_back(may_write_in);
          unwind_must_read_results.emplace_back();
          unwind_must_write_results.emplace_back();
          unwind_may_depth_results.push_back(may_depth_in);
          unwind_must_depth_results.emplace_back();
        }

        if (unwind_may_results.empty() && !getCallees(invoke).empty()) {
          unwind_may_results.push_back(may_in);
          unwind_must_results.emplace_back();
          unwind_may_read_results.push_back(may_read_in);
          unwind_may_write_results.push_back(may_write_in);
          unwind_must_read_results.emplace_back();
          unwind_must_write_results.emplace_back();
          unwind_may_depth_results.push_back(may_depth_in);
          unwind_must_depth_results.emplace_back();
        }

        InvokeEdgeFacts unwind_facts;
        if (!unwind_may_results.empty()) {
          unwind_facts.may_lockset = merge(unwind_may_results, false);
          unwind_facts.must_lockset = merge(unwind_must_results, true);
          unwind_facts.may_read_lockset = merge(unwind_may_read_results, false);
          unwind_facts.may_write_lockset =
              merge(unwind_may_write_results, false);
          unwind_facts.must_read_lockset =
              merge(unwind_must_read_results, true);
          unwind_facts.must_write_lockset =
              merge(unwind_must_write_results, true);
          unwind_facts.may_recursive_depth =
              mergeDepths(unwind_may_depth_results, false);
          unwind_facts.must_recursive_depth =
              mergeDepths(unwind_must_depth_results, true);
          may_out.insert(unwind_facts.may_lockset.begin(),
                         unwind_facts.may_lockset.end());
          may_read_out.insert(unwind_facts.may_read_lockset.begin(),
                              unwind_facts.may_read_lockset.end());
          may_write_out.insert(unwind_facts.may_write_lockset.begin(),
                               unwind_facts.may_write_lockset.end());
          must_out = merge({must_out, unwind_facts.must_lockset}, true);
          must_read_out =
              merge({must_read_out, unwind_facts.must_read_lockset}, true);
          must_write_out =
              merge({must_write_out, unwind_facts.must_write_lockset}, true);
          may_depth_out = mergeDepths(
              {may_depth_out, unwind_facts.may_recursive_depth}, false);
          must_depth_out = mergeDepths(
              {must_depth_out, unwind_facts.must_recursive_depth}, true);
        }
        m_invoke_unwind_exit[inst] = std::move(unwind_facts);
      }
    }

    bool had_entry = m_may_locksets_entry.count(inst);
    bool changed = !had_entry || may_ownership_entry_changed ||
                   must_ownership_entry_changed;
    if (m_may_recursive_depth_entry[inst] != may_depth_in) {
      m_may_recursive_depth_entry[inst] = may_depth_in;
      changed = true;
    }
    if (m_may_recursive_depth_exit[inst] != may_depth_out) {
      m_may_recursive_depth_exit[inst] = may_depth_out;
      changed = true;
    }
    if (m_must_recursive_depth_entry[inst] != must_depth_in) {
      m_must_recursive_depth_entry[inst] = must_depth_in;
      changed = true;
    }
    if (m_must_recursive_depth_exit[inst] != must_depth_out) {
      m_must_recursive_depth_exit[inst] = must_depth_out;
      changed = true;
    }
    if (m_may_raii_ownership_exit[inst] != may_ownership_out) {
      m_may_raii_ownership_exit[inst] = may_ownership_out;
      changed = true;
    }
    if (m_must_raii_ownership_exit[inst] != must_ownership_out) {
      m_must_raii_ownership_exit[inst] = must_ownership_out;
      changed = true;
    }
    if (m_may_read_locks_entry[inst] != may_read_in) {
      m_may_read_locks_entry[inst] = may_read_in;
      changed = true;
    }
    if (m_may_read_locks_exit[inst] != may_read_out) {
      m_may_read_locks_exit[inst] = may_read_out;
      changed = true;
    }
    if (m_may_write_locks_entry[inst] != may_write_in) {
      m_may_write_locks_entry[inst] = may_write_in;
      changed = true;
    }
    if (m_may_write_locks_exit[inst] != may_write_out) {
      m_may_write_locks_exit[inst] = may_write_out;
      changed = true;
    }
    if (m_must_read_locks_entry[inst] != must_read_in) {
      m_must_read_locks_entry[inst] = must_read_in;
      changed = true;
    }
    if (m_must_read_locks_exit[inst] != must_read_out) {
      m_must_read_locks_exit[inst] = must_read_out;
      changed = true;
    }
    if (m_must_write_locks_entry[inst] != must_write_in) {
      m_must_write_locks_entry[inst] = must_write_in;
      changed = true;
    }
    if (m_must_write_locks_exit[inst] != must_write_out) {
      m_must_write_locks_exit[inst] = must_write_out;
      changed = true;
    }
    if (m_may_locksets_entry[inst] != may_in) {
      m_may_locksets_entry[inst] = may_in;
      changed = true;
    }
    if (m_may_locksets_exit[inst] != may_out) {
      m_may_locksets_exit[inst] = may_out;
      changed = true;
    }
    if (m_must_locksets_entry[inst] != must_in) {
      m_must_locksets_entry[inst] = must_in;
      changed = true;
    }
    if (m_must_locksets_exit[inst] != must_out) {
      m_must_locksets_exit[inst] = must_out;
      changed = true;
    }

    if (!changed) {
      continue;
    }
    if (const Instruction *next = inst->getNextNode()) {
      if (in_worklist.find(next) == in_worklist.end()) {
        worklist.push(next);
        in_worklist.insert(next);
      }
      continue;
    }
    if (!inst->isTerminator()) {
      continue;
    }
    for (const BasicBlock *succ_bb : successors(inst->getParent())) {
      const Instruction *succ = &succ_bb->front();
      if (in_worklist.find(succ) == in_worklist.end()) {
        worklist.push(succ);
        in_worklist.insert(succ);
      }
    }
  }
}

LockSet LockSetAnalysis::transfer(const Instruction *inst,
                                  const LockSet &in_set, bool is_must) const {
  LockSet out_set = in_set;
  auto eraseReleasedLocks = [&](const std::vector<LockID> &locks) {
    for (LockID lock : locks) {
      out_set.erase(lock);
      if (is_must) {
        LockSet to_remove;
        for (const auto *held : out_set) {
          if (mayAlias(held, lock)) {
            to_remove.insert(held);
          }
        }
        for (const auto *held : to_remove) {
          out_set.erase(held);
        }
      }
    }
  };

  std::vector<LockID> raii_releases =
      getRAIILocksReleasedAt(inst, is_must);
  if (!raii_releases.empty()) {
    eraseReleasedLocks(raii_releases);
    return out_set;
  }
  if (is_must) {
    eraseReleasedLocks(getImpreciseRAIILocksEndingAt(inst));
  }

  const CallBase *call = dyn_cast<CallBase>(inst);
  ThreadAPI::TD_TYPE call_type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return out_set;
  }
  const bool raw_lock_api = call_type == ThreadAPI::TD_ACQUIRE ||
                            call_type == ThreadAPI::TD_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_RWLOCK_RDLOCK ||
                            call_type == ThreadAPI::TD_RWLOCK_WRLOCK ||
                            call_type == ThreadAPI::TD_CPP_LOCK_TRY ||
                            call_type == ThreadAPI::TD_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_ACQUIRE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN ||
                            call_type == ThreadAPI::TD_KERNEL_READ_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_READ ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_WRITE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP ||
                            call_type == ThreadAPI::TD_KERNEL_READ_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP_READ ||
                            call_type == ThreadAPI::TD_KERNEL_UP_WRITE;

  if (call_type == ThreadAPI::TD_CPP_LOCK_RELEASE ||
      call_type == ThreadAPI::TD_CPP_LOCK_MOVE_CTOR ||
      call_type == ThreadAPI::TD_CPP_LOCK_SWAP) {
    return out_set;
  }
  if (call_type == ThreadAPI::TD_CPP_LOCK_MOVE_ASSIGN) {
    if (LockID lock = getCppWrapperLockValue(inst))
      out_set.erase(lock);
    return out_set;
  }

  if (m_thread_api->isTDAcquire(inst) && raw_lock_api) {
    if (!m_thread_api->isConditionalLockAcquire(inst) || !is_must) {
      if (LockID lock = getLockValue(inst)) {
        out_set.insert(lock);
      }
    }
  } else if (m_thread_api->isTDRelease(inst)) {
    if (LockID lock = getLockValue(inst)) {
      out_set.erase(lock);
      if (is_must) {
        LockSet to_remove;
        for (const auto *l : out_set) {
          if (mayAlias(l, lock)) {
            to_remove.insert(l);
          }
        }
        for (const auto *l : to_remove) {
          out_set.erase(l);
        }
      }
    }
  } else if (m_thread_api->isTDCondWait(inst)) {
  } else if (call) {
    ThreadAPI::TD_TYPE type = call_type;

    switch (type) {
    case ThreadAPI::TD_SHARED_RDLOCK:
    case ThreadAPI::TD_SHARED_WRLOCK:
      if (LockID lock = getLockValue(inst))
        out_set.insert(lock);
      return out_set;

    case ThreadAPI::TD_SHARED_UNLOCK:
      if (LockID lock = getLockValue(inst)) {
        out_set.erase(lock);
        if (is_must) {
          LockSet to_remove;
          for (const auto *l : out_set)
            if (mayAlias(l, lock))
              to_remove.insert(l);
          for (const auto *l : to_remove)
            out_set.erase(l);
        }
      }
      return out_set;

    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    case ThreadAPI::TD_SHARED_LOCK_CTOR: {
      auto shouldAddAtCtor = [&](LockID lock,
                                 RAIILock::OwnershipKind ownership) {
        switch (ownership) {
        case RAIILock::OwnershipKind::Immediate:
          return true;
        case RAIILock::OwnershipKind::Deferred:
          return false;
        case RAIILock::OwnershipKind::Try:
          return !is_must;
        case RAIILock::OwnershipKind::Adopt:
          for (const auto *held : in_set) {
            if (held == lock || mayAlias(held, lock)) {
              return true;
            }
          }
          return false;
        case RAIILock::OwnershipKind::Unknown:
          return !is_must;
        }
        return false;
      };

      const Function *parent_func = inst->getFunction();
      auto raii_it = m_raii_locks.find(parent_func);
      if (raii_it != m_raii_locks.end()) {
        for (const auto &raii_entry : raii_it->second) {
          const RAIILock::LockLifetime &lifetime = raii_entry.second;
          if (lifetime.constructor == call &&
              !lifetime.underlyingLocks.empty()) {
            for (const Value *underlying : lifetime.underlyingLocks) {
              if (LockID lock = getCanonicalLock(underlying)) {
                if (shouldAddAtCtor(lock, lifetime.ownership)) {
                  out_set.insert(lock);
                }
              }
            }
            return out_set;
          }
        }
      }

      RAIILock::OwnershipKind fallback_ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      for (const Value *underlying :
           RAIILock::RAIILockTracker::extractUnderlyingLocks(call)) {
        if (LockID lock = getCanonicalLock(underlying)) {
          if (shouldAddAtCtor(lock, fallback_ownership)) {
            out_set.insert(lock);
          }
        }
      }
      return out_set;
    }

    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_SHARED_LOCK_DTOR: {
      // Precise wrapper-owned releases were handled by
      // getRAIILocksReleasedAt() above. An empty result means this destructor
      // is a no-op for the modeled wrapper state (deferred, released,
      // moved-from, or otherwise not definitely owning).
      return out_set;
    }

    case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.insert(lock);
      }
      return out_set;

    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.erase(lock);
        if (is_must) {
          LockSet to_remove;
          for (const auto *l : out_set)
            if (mayAlias(l, lock))
              to_remove.insert(l);
          for (const auto *l : to_remove)
            out_set.erase(l);
        }
      }
      return out_set;

    case ThreadAPI::TD_CPP_LOCK_RELEASE:
      // release() detaches ownership without unlocking; the mutex remains
      // held, so the program lock fact is unchanged.
      return out_set;

    case ThreadAPI::TD_CPP_LOCK_MOVE_CTOR:
    case ThreadAPI::TD_CPP_LOCK_SWAP:
      // Wrapper state changes do not acquire or release the underlying mutex.
      // Destructor filtering consults the wrapper transition history.
      return out_set;

    case ThreadAPI::TD_CPP_LOCK_MOVE_ASSIGN:
      // Move assignment releases any mutex previously owned by the
      // destination before transferring the source wrapper state.
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.erase(lock);
      }
      return out_set;

    case ThreadAPI::TD_CALL_ONCE:
    case ThreadAPI::TD_FUTURE_GET:
    case ThreadAPI::TD_FUTURE_WAIT:
    case ThreadAPI::TD_PROMISE_SET:
    case ThreadAPI::TD_LATCH_WAIT:
    case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
    case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
    case ThreadAPI::TD_BARRIER_WAIT_CPP20:
    case ThreadAPI::TD_OMP_TASKWAIT:
    case ThreadAPI::TD_OMP_TASKWAIT_DEPS:
    case ThreadAPI::TD_OMP_TASKGROUP_END:
    case ThreadAPI::TD_OMP_FLUSH:
      return out_set;

    default:
      break;
    }

    if (!m_thread_api->isTDAcquire(call) && !m_thread_api->isTDRelease(call) &&
        !m_thread_api->isTDCondWait(call)) {
      auto callees = getCallees(call);
      if (callees.empty()) {
        if (is_must && shouldInvalidateMustLockState(call)) {
          out_set.clear();
        }
        return out_set;
      }
      std::vector<LockSet> callee_results;
      for (Function *callee : callees) {
        if (!callee || callee->isDeclaration())
          continue;
        LockSet candidate = out_set;
        auto it = m_function_summaries.find(callee);
        if (it != m_function_summaries.end() && it->second.is_analyzed) {
          if (!is_must) {
            LockSet may_only = candidate;
            LockSet must_dummy = candidate;
            applyFunctionSummary(call, callee, may_only, must_dummy);
            candidate = std::move(may_only);
          } else {
            LockSet may_dummy = candidate;
            LockSet must_only = candidate;
            applyFunctionSummary(call, callee, may_dummy, must_only);
            candidate = std::move(must_only);
          }
        }
        callee_results.push_back(std::move(candidate));
      }
      if (!callee_results.empty()) {
        out_set = merge(callee_results, is_must);
      }
      if (is_must && shouldInvalidateMustLockState(call)) {
        out_set.clear();
      }
    }
  }

  return out_set;
}

void LockSetAnalysis::transferReadWrite(const Instruction *inst,
                                        const LockSet &in_read,
                                        const LockSet &in_write,
                                        LockSet &out_read, LockSet &out_write,
                                        bool is_must) const {
  out_read = in_read;
  out_write = in_write;
  auto eraseReleasedLocks = [&](const std::vector<LockID> &locks) {
    for (LockID lock : locks) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *held : out_read) {
          if (mayAlias(held, lock)) {
            to_remove_r.insert(held);
          }
        }
        for (const auto *held : out_write) {
          if (mayAlias(held, lock)) {
            to_remove_w.insert(held);
          }
        }
        for (const auto *held : to_remove_r) {
          out_read.erase(held);
        }
        for (const auto *held : to_remove_w) {
          out_write.erase(held);
        }
      }
    }
  };

  std::vector<LockID> raii_releases =
      getRAIILocksReleasedAt(inst, is_must);
  if (!raii_releases.empty()) {
    eraseReleasedLocks(raii_releases);
    return;
  }
  if (is_must) {
    eraseReleasedLocks(getImpreciseRAIILocksEndingAt(inst));
  }

  const CallBase *call = dyn_cast<CallBase>(inst);
  ThreadAPI::TD_TYPE call_type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (detail::isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return;
  }
  const bool raw_lock_api = call_type == ThreadAPI::TD_ACQUIRE ||
                            call_type == ThreadAPI::TD_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_RWLOCK_RDLOCK ||
                            call_type == ThreadAPI::TD_RWLOCK_WRLOCK ||
                            call_type == ThreadAPI::TD_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_ACQUIRE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN ||
                            call_type == ThreadAPI::TD_KERNEL_READ_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_READ ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_WRITE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP ||
                            call_type == ThreadAPI::TD_KERNEL_READ_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP_READ ||
                            call_type == ThreadAPI::TD_KERNEL_UP_WRITE;

  if (m_thread_api->isReadLockAcquire(inst)) {
    if (!m_thread_api->isConditionalLockAcquire(inst) || !is_must) {
      if (LockID lock = getLockValue(inst))
        out_read.insert(lock);
    }
  } else if (m_thread_api->isWriteLockAcquire(inst)) {
    if (!m_thread_api->isConditionalLockAcquire(inst) || !is_must) {
      if (LockID lock = getLockValue(inst)) {
        out_write.insert(lock);
        for (const auto *l : in_read) {
          if ((is_must && mayAlias(l, lock)) ||
              (!is_must && locksMustMatch(l, lock))) {
            out_read.erase(l);
          }
        }
      }
    }
  } else if (m_thread_api->isTDAcquire(inst) && raw_lock_api) {
    if (!m_thread_api->isConditionalLockAcquire(inst) || !is_must) {
      if (LockID lock = getLockValue(inst))
        out_write.insert(lock);
    }
  } else if (m_thread_api->isTDCondWait(inst)) {
  } else if (m_thread_api->isTDRelease(inst)) {
    if (LockID lock = getLockValue(inst)) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *l : out_read)
          if (mayAlias(l, lock))
            to_remove_r.insert(l);
        for (const auto *l : out_write)
          if (mayAlias(l, lock))
            to_remove_w.insert(l);
        for (const auto *l : to_remove_r)
          out_read.erase(l);
        for (const auto *l : to_remove_w)
          out_write.erase(l);
      }
    }
  } else if (call) {
    ThreadAPI::TD_TYPE type = call_type;

    switch (type) {
    case ThreadAPI::TD_SHARED_RDLOCK:
      if (call->arg_size() >= 1) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(0)))
          out_read.insert(lock);
      }
      return;

    case ThreadAPI::TD_SHARED_WRLOCK:
      if (call->arg_size() >= 1) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(0))) {
          out_write.insert(lock);
          for (const auto *l : in_read) {
            if ((is_must && mayAlias(l, lock)) ||
                (!is_must && locksMustMatch(l, lock))) {
              out_read.erase(l);
            }
          }
        }
      }
      return;

    case ThreadAPI::TD_SHARED_UNLOCK:
      if (call->arg_size() >= 1) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(0))) {
          out_read.erase(lock);
          out_write.erase(lock);
          if (is_must) {
            LockSet to_remove_r, to_remove_w;
            for (const auto *l : out_read)
              if (mayAlias(l, lock))
                to_remove_r.insert(l);
            for (const auto *l : out_write)
              if (mayAlias(l, lock))
                to_remove_w.insert(l);
            for (const auto *l : to_remove_r)
              out_read.erase(l);
            for (const auto *l : to_remove_w)
              out_write.erase(l);
          }
        }
      }
      return;

    case ThreadAPI::TD_SHARED_LOCK_CTOR: {
      RAIILock::OwnershipKind ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      bool should_add =
          ownership == RAIILock::OwnershipKind::Immediate ||
          (!is_must && (ownership == RAIILock::OwnershipKind::Try ||
                        ownership == RAIILock::OwnershipKind::Unknown));
      if (!should_add && ownership != RAIILock::OwnershipKind::Adopt) {
        return;
      }
      for (const Value *underlying :
           RAIILock::RAIILockTracker::extractUnderlyingLocks(call)) {
        if (LockID lock = getCanonicalLock(underlying)) {
          if (ownership == RAIILock::OwnershipKind::Adopt) {
            bool held = false;
            for (const auto *candidate : in_read) {
              if (mayAlias(candidate, lock)) {
                held = true;
                break;
              }
            }
            if (!held) {
              continue;
            }
          }
          out_read.insert(lock);
        }
      }
      return;
    }

    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR: {
      RAIILock::OwnershipKind ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      bool should_add =
          ownership == RAIILock::OwnershipKind::Immediate ||
          (!is_must && (ownership == RAIILock::OwnershipKind::Try ||
                        ownership == RAIILock::OwnershipKind::Unknown));
      if (ownership == RAIILock::OwnershipKind::Deferred) {
        should_add = false;
      }
      if (!should_add && ownership != RAIILock::OwnershipKind::Adopt) {
        return;
      }
      for (const Value *underlying :
           RAIILock::RAIILockTracker::extractUnderlyingLocks(call)) {
        if (LockID lock = getCanonicalLock(underlying)) {
          if (ownership == RAIILock::OwnershipKind::Adopt) {
            bool held = false;
            for (const auto *candidate : in_write) {
              if (mayAlias(candidate, lock)) {
                held = true;
                break;
              }
            }
            if (!held) {
              continue;
            }
          }
          out_write.insert(lock);
        }
      }
      return;
    }

    case ThreadAPI::TD_SHARED_LOCK_DTOR:
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK: {
      if (call_type != ThreadAPI::TD_UNIQUE_LOCK_UNLOCK &&
          getRAIILocksReleasedAt(inst, is_must).empty()) {
        return;
      }
      std::vector<LockID> locks =
          getUnderlyingRAIILocks(inst, call->getArgOperand(0));
      if (locks.empty()) {
        if (LockID lock = getCppWrapperLockValue(inst)) {
          locks.push_back(lock);
        }
      }
      if (locks.empty() && is_must) {
        out_read.clear();
        out_write.clear();
        break;
      }
      for (LockID lock : locks) {
        out_read.erase(lock);
        out_write.erase(lock);
        if (is_must) {
          LockSet to_remove_r, to_remove_w;
          for (const auto *l : out_read)
            if (mayAlias(l, lock))
              to_remove_r.insert(l);
          for (const auto *l : out_write)
            if (mayAlias(l, lock))
              to_remove_w.insert(l);
          for (const auto *l : to_remove_r)
            out_read.erase(l);
          for (const auto *l : to_remove_w)
            out_write.erase(l);
        }
      }
      return;
    }

    case ThreadAPI::TD_UNIQUE_LOCK_LOCK: {
      std::vector<LockID> locks =
          getUnderlyingRAIILocks(inst, call->getArgOperand(0));
      if (locks.empty()) {
        if (LockID lock = getCppWrapperLockValue(inst)) {
          locks.push_back(lock);
        }
      }
      for (LockID lock : locks) {
        out_write.insert(lock);
      }
      return;
    }

    case ThreadAPI::TD_CPP_LOCK_RELEASE:
    case ThreadAPI::TD_CPP_LOCK_MOVE_CTOR:
    case ThreadAPI::TD_CPP_LOCK_SWAP:
      return;

    case ThreadAPI::TD_CPP_LOCK_MOVE_ASSIGN:
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_read.erase(lock);
        out_write.erase(lock);
      }
      return;

    default:
      break;
    }

    auto applySummaryToReadWrite = [&](const Function *callee,
                                       LockSet &candidate_read,
                                       LockSet &candidate_write) -> bool {
      auto it = m_function_summaries.find(callee);
      if (it == m_function_summaries.end() || !it->second.is_analyzed) {
        return false;
      }

      auto eraseMustReleasedLock = [&](LockID released_lock) {
        if (!released_lock) {
          return;
        }
        candidate_read.erase(released_lock);
        candidate_write.erase(released_lock);
        LockSet aliased_read;
        LockSet aliased_write;
        for (const auto *held : candidate_read) {
          if (mayAlias(held, released_lock)) {
            aliased_read.insert(held);
          }
        }
        for (const auto *held : candidate_write) {
          if (mayAlias(held, released_lock)) {
            aliased_write.insert(held);
          }
        }
        for (const auto *held : aliased_read) {
          candidate_read.erase(held);
        }
        for (const auto *held : aliased_write) {
          candidate_write.erase(held);
        }
      };

      const FunctionSummary &summary = it->second;
      if (!is_must) {
        for (LockID lock : summary.may_read_acquire_delta) {
          if (LockID instantiated =
                  instantiateSummaryLock(call, callee, lock)) {
            candidate_read.insert(instantiated);
          }
        }
        for (LockID lock : summary.may_write_acquire_delta) {
          if (LockID instantiated =
                  instantiateSummaryLock(call, callee, lock)) {
            candidate_write.insert(instantiated);
          }
        }
      } else {
        for (LockID lock : summary.must_read_acquire_delta) {
          if (LockID instantiated =
                  instantiateSummaryLock(call, callee, lock)) {
            candidate_read.insert(instantiated);
          }
        }
        for (LockID lock : summary.must_write_acquire_delta) {
          if (LockID instantiated =
                  instantiateSummaryLock(call, callee, lock)) {
            candidate_write.insert(instantiated);
          }
        }
        for (LockID lock : summary.may_release_delta) {
          eraseMustReleasedLock(instantiateSummaryLock(call, callee, lock));
        }
      }

      for (LockID lock : summary.must_release_delta) {
        eraseMustReleasedLock(instantiateSummaryLock(call, callee, lock));
      }
      return true;
    };

    auto callees = getCallees(call);
    std::vector<LockSet> read_results;
    std::vector<LockSet> write_results;
    for (Function *callee : callees) {
      if (!callee || callee->isDeclaration()) {
        continue;
      }
      LockSet candidate_read = out_read;
      LockSet candidate_write = out_write;
      (void)applySummaryToReadWrite(callee, candidate_read, candidate_write);
      read_results.push_back(std::move(candidate_read));
      write_results.push_back(std::move(candidate_write));
    }

    if (!read_results.empty()) {
      out_read = merge(read_results, is_must);
      out_write = merge(write_results, is_must);
    }

    if (is_must && shouldInvalidateMustLockState(call)) {
      out_read.clear();
      out_write.clear();
    }
  }
}

LockSet LockSetAnalysis::merge(const std::vector<LockSet> &sets,
                               bool is_must) const {
  if (sets.empty()) {
    return LockSet();
  }

  if (is_must) {
    auto matchesLock = [this](LockID lhs, LockID rhs) {
      const LockID clhs = getCanonicalLock(lhs);
      const LockID crhs = getCanonicalLock(rhs);
      if (clhs && crhs && clhs == crhs) {
        return true;
      }
      return m_alias_analysis && clhs && crhs &&
             m_alias_analysis->mustAlias(clhs, crhs);
    };

    LockSet result = sets[0];
    for (size_t i = 1; i < sets.size(); ++i) {
      LockSet intersection;
      for (LockID lhs : result) {
        for (LockID rhs : sets[i]) {
          if (matchesLock(lhs, rhs)) {
            intersection.insert(getCanonicalLock(lhs));
            break;
          }
        }
      }
      result = intersection;
    }
    return result;
  }

  LockSet result;
  for (const auto &set : sets) {
    result.insert(set.begin(), set.end());
  }
  return result;
}

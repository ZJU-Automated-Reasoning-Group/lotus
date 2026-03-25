/**
 * @file StaticVectorClockMHP.cpp
 * @brief Implementation of the static vector clock based MHP analysis (CGO 1).
 */

#include "Analysis/Concurrency/MHP/StaticVectorClockMHP.h"

#include <deque>
#include <set>
#include <unordered_set>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace {
bool isLikelyThreadEntryCandidate(const Function &func) {
  if (func.isDeclaration() || func.arg_size() > 1) {
    return false;
  }
  if (func.arg_size() == 1 &&
      !func.getFunctionType()->getParamType(0)->isPointerTy()) {
    return false;
  }
  Type *retTy = func.getReturnType();
  return retTy->isPointerTy() || retTy->isVoidTy() || retTy->isIntegerTy();
}
} // namespace

StaticVectorClockMHP::StaticVectorClockMHP(Module &module)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()) {
  m_tfg = std::make_unique<ThreadFlowGraph>();
}

void StaticVectorClockMHP::analyze() {
  m_ctx_to_stid.clear();
  m_static_threads.clear();
  m_sync_node_to_static_nodes.clear();
  m_inst_to_static_nodes.clear();
  m_static_node_predecessors.clear();
  m_static_node_successors.clear();
  m_static_node_keys.clear();
  m_static_node_clocks.clear();
  m_mhp_pairs.clear();
  m_thread_instruction_cache.clear();
  m_parallel_instruction_cache.clear();
  m_reachable_node_ids.clear();
  m_static_node_to_id.clear();
  m_static_node_by_id.clear();
  m_ret_to_call.clear();
  m_inst_to_thread.clear();
  m_thread_fork_sites.clear();
  m_thread_parents.clear();
  m_thread_children.clear();
  m_fork_to_thread.clear();
  m_join_to_thread.clear();
  m_detached_threads.clear();
  m_pthread_value_to_threads.clear();
  m_thread_to_pthread_value.clear();
  m_visited_functions_by_thread.clear();
  m_active_call_stack_by_thread.clear();
  m_condvar_signals.clear();
  m_condvar_waits.clear();
  m_barrier_waits.clear();
  m_barrier_phase_by_thread.clear();
  m_pending_split_barrier_phase_by_thread.clear();
  m_barrier_expected_counts.clear();
  m_multi_instance_threads.clear();
  m_thread_entry_candidates.clear();
  m_has_unresolved_fork = false;
  m_next_thread_id = 1;
  m_post_dom_cache.clear();
  m_tfg = std::make_unique<ThreadFlowGraph>();
  m_call_graph = std::make_unique<CallGraph>(m_module);
  m_join_target_analysis = std::make_unique<JoinTargetAnalysis>(m_module);
  m_thread_multiplicity =
      std::make_unique<concurrency::ThreadMultiplicityAnalysis>(
          m_module, m_call_graph.get());
  buildThreadFlowGraph();
  buildStaticThreads();
  buildStaticEdges();
  computeReachabilityPerStaticThread();
  initializeNodeClocks();
  computeStaticVectorClocks();
  computeMHPPairs();
}

// === StaticVectorClock helpers =============================================

bool StaticVectorClockMHP::StaticVectorClock::mergeFrom(
    const StaticVectorClock &other) {
  bool changed = false;
  for (const auto &kv : other.entries) {
    auto &dest = entries[kv.first];
    for (const auto &elem : kv.second) {
      changed |= dest.insert(elem).second;
    }
  }
  return changed;
}

bool StaticVectorClockMHP::StaticVectorClock::leq(
    const StaticVectorClock &other) const {
  for (const auto &kv : entries) {
    auto it = other.entries.find(kv.first);
    if (it == other.entries.end()) {
      if (!kv.second.empty())
        return false;
      continue;
    }
    const auto &rhs = it->second;
    for (const auto &elem : kv.second) {
      if (rhs.find(elem) == rhs.end())
        return false;
    }
  }
  return true;
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::initialClockFor(const StaticThread &st) const {
  StaticVectorClock init;
  init.entries[st.id].insert({LogicClockElem::Kind::Start, 0});
  // Paper: SV = {T_main → {S}, T → {⊤}} for others. ⊤ = not yet started.
  LogicClockElem::Kind otherKind = LogicClockElem::Kind::Top;
  for (const auto &s : m_static_threads) {
    if (s.id != st.id)
      init.entries[s.id].insert({otherKind, 0});
  }
  return init;
}

StaticThreadID StaticVectorClockMHP::getOrCreateStaticThread(
    const Context &ctx, ThreadID base_tid, const SyncNode *entry) {
  auto it = m_ctx_to_stid.find(ctx);
  if (it != m_ctx_to_stid.end())
    return it->second;

  StaticThreadID new_id = m_static_threads.size();
  StaticThread st;
  st.id = new_id;
  st.ctx = ctx;
  st.base_tid = base_tid;
  st.entry = entry;
  m_static_threads.push_back(st);
  m_ctx_to_stid[ctx] = new_id;
  return new_id;
}

void StaticVectorClockMHP::buildStaticThreads() {
  m_static_threads.clear();
  m_ctx_to_stid.clear();
  m_sync_node_to_static_nodes.clear();
  m_inst_to_static_nodes.clear();
  m_static_node_predecessors.clear();
  m_static_node_successors.clear();
  m_static_node_keys.clear();
  m_static_node_clocks.clear();
  m_static_node_to_id.clear();
  m_static_node_by_id.clear();

  if (!m_tfg)
    return;

  const SyncNode *main_entry = m_tfg->getThreadEntryNode(0);
  Context root;
  root.call_sites.clear();
  root.fork_sites.clear();
  StaticThreadID root_id = getOrCreateStaticThread(root, 0, main_entry);

  using WorkItem =
      std::tuple<StaticThreadID, const SyncNode *, std::vector<size_t>>;
  std::deque<WorkItem> worklist;
  worklist.push_back({root_id, main_entry, {}});

  using VisitKey = std::tuple<StaticThreadID, const SyncNode *, CallString>;
  std::set<VisitKey> visited;
  while (!worklist.empty()) {
    StaticThreadID stid;
    const SyncNode *node;
    std::vector<size_t> call_sites;
    std::tie(stid, node, call_sites) = worklist.front();
    worklist.pop_front();

    if (!node || !visited.insert({stid, node, call_sites}).second)
      continue;

    StaticThread &st = m_static_threads[stid];
    StaticNodeKey key{node, stid, call_sites};
    if (!m_static_node_to_id.count(key)) {
      const size_t static_node_id = m_static_node_by_id.size();
      m_static_node_to_id[key] = static_node_id;
      m_static_node_by_id.push_back(key);
      st.nodes.push_back(key);
      m_static_node_keys.push_back(key);
      m_sync_node_to_static_nodes[node].push_back(key);
      if (const Instruction *inst = node->getInstruction()) {
        m_inst_to_static_nodes[inst].push_back(key);
      }
    }

    for (SyncNode *succ : node->getSuccessors()) {
      EdgeKind kind = m_tfg->getEdgeKind(node, succ);
      std::vector<size_t> new_call_sites = call_sites;
      if (kind == EdgeKind::Call) {
        if (new_call_sites.size() >= kCallContextLimit)
          new_call_sites.erase(new_call_sites.begin());
        new_call_sites.push_back(node->getNodeID());
      } else if (kind == EdgeKind::Ret) {
        if (!new_call_sites.empty())
          new_call_sites.pop_back();
      }
      if (succ->getThreadID() == st.base_tid) {
        worklist.push_back({stid, succ, new_call_sites});
      } else if (kind == EdgeKind::Create) {
        Context child_ctx;
        child_ctx.call_sites = call_sites;
        child_ctx.fork_sites = st.ctx.fork_sites;
        child_ctx.fork_sites.push_back(node->getNodeID());
        StaticThreadID child_stid =
            getOrCreateStaticThread(child_ctx, succ->getThreadID(), succ);
        worklist.push_back({child_stid, succ, {}});
      }
    }
  }
}

void StaticVectorClockMHP::buildStaticEdges() {
  m_static_node_predecessors.clear();
  m_static_node_successors.clear();

  auto pushUnique = [](StaticNodeList &list, const StaticNodeKey &key) {
    if (std::find(list.begin(), list.end(), key) == list.end()) {
      list.push_back(key);
    }
  };
  auto addEdge = [&](const StaticNodeKey &from, const StaticNodeKey &to) {
    pushUnique(m_static_node_successors[from], to);
    pushUnique(m_static_node_predecessors[to], from);
  };

  for (const StaticNodeKey &current : m_static_node_keys) {
    if (!current.node || current.stid >= m_static_threads.size()) {
      continue;
    }
    const StaticThread &st = m_static_threads[current.stid];

    for (SyncNode *succ : current.node->getSuccessors()) {
      const EdgeKind kind = m_tfg->getEdgeKind(current.node, succ);

      if (succ->getThreadID() == st.base_tid) {
        CallString new_call_sites = current.call_sites;
        if (kind == EdgeKind::Call) {
          if (new_call_sites.size() >= kCallContextLimit) {
            new_call_sites.erase(new_call_sites.begin());
          }
          new_call_sites.push_back(current.node->getNodeID());
        } else if (kind == EdgeKind::Ret) {
          auto rit = m_ret_to_call.find(succ);
          if (rit == m_ret_to_call.end() || new_call_sites.empty() ||
              new_call_sites.back() != rit->second->getNodeID()) {
            continue;
          }
          new_call_sites.pop_back();
        }

        StaticNodeKey target{succ, current.stid, new_call_sites};
        if (m_static_node_to_id.count(target)) {
          addEdge(current, target);
        }
        continue;
      }

      if (kind == EdgeKind::Create) {
        Context child_ctx;
        child_ctx.call_sites = current.call_sites;
        child_ctx.fork_sites = st.ctx.fork_sites;
        child_ctx.fork_sites.push_back(current.node->getNodeID());
        auto child_it = m_ctx_to_stid.find(child_ctx);
        if (child_it == m_ctx_to_stid.end()) {
          continue;
        }
        StaticNodeKey target{succ, child_it->second, {}};
        if (m_static_node_to_id.count(target)) {
          addEdge(current, target);
        }
        continue;
      }

      if (kind == EdgeKind::Join) {
        std::vector<size_t> parent_forks = st.ctx.fork_sites;
        if (!parent_forks.empty()) {
          parent_forks.pop_back();
        }
        const auto succ_it = m_sync_node_to_static_nodes.find(succ);
        if (succ_it == m_sync_node_to_static_nodes.end()) {
          continue;
        }
        for (const StaticNodeKey &candidate : succ_it->second) {
          const StaticThread &succ_st = m_static_threads[candidate.stid];
          if (succ_st.base_tid != succ->getThreadID()) {
            continue;
          }
          if (succ_st.ctx.call_sites == st.ctx.call_sites &&
              succ_st.ctx.fork_sites == parent_forks &&
              candidate.call_sites == st.ctx.call_sites) {
            addEdge(current, candidate);
          }
        }
        continue;
      }

      if (kind == EdgeKind::Signal || kind == EdgeKind::Barrier) {
        const auto succ_it = m_sync_node_to_static_nodes.find(succ);
        if (succ_it == m_sync_node_to_static_nodes.end()) {
          continue;
        }
        for (const StaticNodeKey &candidate : succ_it->second) {
          const StaticThread &succ_st = m_static_threads[candidate.stid];
          if (succ_st.base_tid != succ->getThreadID()) {
            continue;
          }
          if (succ_st.ctx.call_sites == st.ctx.call_sites) {
            addEdge(current, candidate);
          }
        }
      }
    }
  }
}

void StaticVectorClockMHP::initializeNodeClocks() {
  for (const StaticNodeKey &key : m_static_node_keys) {
    if (key.stid < m_static_threads.size()) {
      m_static_node_clocks[key] = initialClockFor(m_static_threads[key.stid]);
    }
  }
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::mergePredecessorClocks(const StaticNodeKey &key) const {
  StaticVectorClock merged;
  if (!key.node)
    return merged;

  auto pred_it = m_static_node_predecessors.find(key);
  if (pred_it == m_static_node_predecessors.end()) {
    return merged;
  }
  for (const StaticNodeKey &pred_key : pred_it->second) {
    auto it = m_static_node_clocks.find(pred_key);
    if (it != m_static_node_clocks.end()) {
      merged.mergeFrom(it->second);
    }
  }
  return merged;
}

bool StaticVectorClockMHP::logicClockLeq(const LogicClockElem &a,
                                         const LogicClockElem &b,
                                         StaticThreadID stid) const {
  // Paper partial order: ⊤ ≤ S ≤ n@c ≤ ⊥ (⊤ min, ⊥ max). leq(a,b) = true iff a
  // ≤ b.
  using K = LogicClockElem::Kind;
  if (a.kind == K::Top)
    return true; // ⊤ ≤ anything
  if (b.kind == K::Terminated)
    return true; // anything ≤ ⊥
  if (a.kind == K::Terminated)
    return (b.kind == K::Terminated); // ⊥ ≤ only ⊥
  if (b.kind == K::Top)
    return (a.kind == K::Top); // a ≤ ⊤ only when a = ⊤
  if (a.kind == K::Start && b.kind == K::Start)
    return true;
  if (a.kind == K::Node && b.kind == K::Start)
    return false;
  if (a.kind == K::Start && b.kind == K::Node)
    return true;
  if (a.kind == K::Start && b.kind == K::Terminated)
    return true;
  if (a.kind == K::Node && b.kind == K::Terminated)
    return true;
  if (a.kind == K::Node && b.kind == K::Node) {
    return nodeReachesInStaticThread(a.node_id, b.node_id, stid);
  }
  return false;
}

void StaticVectorClockMHP::logicClockMax(const LogicClockSet &la,
                                         const LogicClockSet &lb,
                                         StaticThreadID stid,
                                         LogicClockSet &out) const {
  for (const auto &a : la) {
    bool dominated = false;
    for (const auto &b : lb) {
      if (logicClockLeq(a, b, stid)) {
        dominated = true;
        break;
      }
    }
    if (!dominated)
      out.insert(a);
  }
  for (const auto &b : lb) {
    bool dominated = false;
    for (const auto &a : la) {
      if (logicClockLeq(b, a, stid)) {
        dominated = true;
        break;
      }
    }
    if (!dominated)
      out.insert(b);
  }
}

bool StaticVectorClockMHP::nodeReachesInStaticThread(
    size_t from_node_id, size_t to_node_id, StaticThreadID stid) const {
  if (from_node_id == to_node_id)
    return true;
  auto it = m_reachable_node_ids.find(stid);
  if (it == m_reachable_node_ids.end())
    return false;
  auto it2 = it->second.find(from_node_id);
  if (it2 == it->second.end())
    return false;
  return it2->second.count(to_node_id) != 0;
}

void StaticVectorClockMHP::computeReachabilityPerStaticThread() {
  m_reachable_node_ids.clear();
  if (!m_tfg)
    return;

  for (const auto &st : m_static_threads) {
    for (const StaticNodeKey &start_key : st.nodes) {
      const auto start_it = m_static_node_to_id.find(start_key);
      if (start_it == m_static_node_to_id.end()) {
        continue;
      }
      const size_t start_id = start_it->second;

      std::unordered_set<size_t> &reached =
          m_reachable_node_ids[st.id][start_id];
      std::deque<StaticNodeKey> worklist;
      std::unordered_set<StaticNodeKey, StaticNodeKeyHash> visited;
      worklist.push_back(start_key);
      visited.insert(start_key);
      reached.insert(start_id);

      while (!worklist.empty()) {
        StaticNodeKey current = worklist.front();
        worklist.pop_front();

        const auto succ_it = m_static_node_successors.find(current);
        if (succ_it == m_static_node_successors.end()) {
          continue;
        }

        for (const StaticNodeKey &succ : succ_it->second) {
          if (succ.stid != st.id) {
            continue;
          }
          const auto succ_id_it = m_static_node_to_id.find(succ);
          if (succ_id_it == m_static_node_to_id.end()) {
            continue;
          }
          reached.insert(succ_id_it->second);
          if (visited.insert(succ).second) {
            worklist.push_back(succ);
          }
        }
      }
    }
  }
}

bool StaticVectorClockMHP::svcLeq(const StaticVectorClock &lhs,
                                  const StaticVectorClock &rhs) const {
  for (const auto &kv : lhs.entries) {
    StaticThreadID stid = kv.first;
    auto it = rhs.entries.find(stid);
    const LogicClockSet &rhs_set =
        (it != rhs.entries.end()) ? it->second : LogicClockSet();
    for (const auto &l1 : kv.second) {
      bool found = false;
      for (const auto &l2 : rhs_set) {
        if (logicClockLeq(l1, l2, stid)) {
          found = true;
          break;
        }
      }
      if (!found && !rhs_set.empty())
        return false;
      if (!found && rhs_set.empty() &&
          l1.kind != LogicClockElem::Kind::Terminated)
        return false;
    }
  }
  return true;
}

void StaticVectorClockMHP::computeSVMax(const StaticVectorClock &sv1,
                                        const StaticVectorClock &sv2,
                                        StaticVectorClock &out) const {
  out.entries.clear();
  for (const auto &kv : sv1.entries) {
    StaticThreadID stid = kv.first;
    auto it = sv2.entries.find(stid);
    if (it == sv2.entries.end()) {
      out.entries[stid] = kv.second;
      continue;
    }
    logicClockMax(kv.second, it->second, stid, out.entries[stid]);
  }
  for (const auto &kv : sv2.entries) {
    if (sv1.entries.count(kv.first))
      continue;
    out.entries[kv.first] = kv.second;
  }
}

std::unordered_set<ThreadID>
StaticVectorClockMHP::getDescendantThreadIDs(ThreadID tid) const {
  std::unordered_set<ThreadID> result;
  std::deque<ThreadID> worklist;
  auto it = m_thread_children.find(tid);
  if (it != m_thread_children.end()) {
    for (ThreadID c : it->second)
      worklist.push_back(c);
  }
  while (!worklist.empty()) {
    ThreadID cur = worklist.front();
    worklist.pop_front();
    if (!result.insert(cur).second)
      continue;
    auto cit = m_thread_children.find(cur);
    if (cit != m_thread_children.end()) {
      for (ThreadID c : cit->second)
        worklist.push_back(c);
    }
  }
  return result;
}

void StaticVectorClockMHP::addEventToClock(const StaticNodeKey &key,
                                           StaticVectorClock &sv) const {
  if (!key.node || key.stid >= m_static_threads.size())
    return;

  const SyncNode *node = key.node;
  StaticThreadID stid = key.stid;
  auto static_node_it = m_static_node_to_id.find(key);
  if (static_node_it != m_static_node_to_id.end()) {
    sv.entries[stid].insert(
        {LogicClockElem::Kind::Node, static_node_it->second});
  }

  if (node->getType() == SyncNodeType::THREAD_EXIT) {
    sv.entries[stid].insert({LogicClockElem::Kind::Terminated, 0});
  }
  // Paper Figure 4 [CREATE]: at fork set T(n2) and all descendant T' to {S}.
  if (node->getType() == SyncNodeType::THREAD_FORK) {
    ThreadID child_tid = node->getForkedThread();
    std::unordered_set<ThreadID> desc_tids = getDescendantThreadIDs(child_tid);
    desc_tids.insert(child_tid);
    for (const auto &st : m_static_threads) {
      if (!desc_tids.count(st.base_tid))
        continue;
      // Direct child: only the static thread created at this fork site.
      // Descendants: all static threads with that base_tid.
      bool isDirectChild = (st.base_tid == child_tid);
      bool matchesFork = !st.ctx.fork_sites.empty() &&
                         st.ctx.fork_sites.back() == node->getNodeID();
      if ((isDirectChild && matchesFork) || (!isDirectChild)) {
        sv.entries[st.id].clear();
        sv.entries[st.id].insert({LogicClockElem::Kind::Start, 0});
      }
    }
  }
}

StaticVectorClockMHP::StaticVectorClock
StaticVectorClockMHP::mergePredecessorClocksWithRules(
    const StaticNodeKey &key) const {
  StaticVectorClock merged;
  if (!key.node)
    return merged;

  // Paper [JOIN]/[SIGNAL]: SVn2@c = SVMax(SVn1@Ø, SVn2@c)[...]. We must first
  // merge all intra-thread predecessors (⊓), then apply SVMax with join/signal.
  auto pred_it = m_static_node_predecessors.find(key);
  if (pred_it == m_static_node_predecessors.end()) {
    return merged;
  }
  for (const StaticNodeKey &pred_key : pred_it->second) {
    auto it = m_static_node_clocks.find(pred_key);
    if (it == m_static_node_clocks.end())
      continue;
    EdgeKind kind = m_tfg->getEdgeKind(pred_key.node, key.node);
    if (kind == EdgeKind::Join || kind == EdgeKind::Signal ||
        kind == EdgeKind::Barrier)
      continue;
    merged.mergeFrom(it->second);
  }
  for (const StaticNodeKey &pred_key : pred_it->second) {
    auto it = m_static_node_clocks.find(pred_key);
    if (it == m_static_node_clocks.end())
      continue;
    EdgeKind kind = m_tfg->getEdgeKind(pred_key.node, key.node);
    if (kind != EdgeKind::Join && kind != EdgeKind::Signal &&
        kind != EdgeKind::Barrier)
      continue;
    StaticVectorClock maxClock;
    computeSVMax(merged, it->second, maxClock);
    merged = std::move(maxClock);
  }
  return merged;
}

bool StaticVectorClockMHP::shouldAddEventAtNode(
    const StaticNodeKey &key) const {
  // Paper Figure 4: [CALL] SV_n2@c' = SV_n1@c (no add); [RET] SV_n2@c =
  // SV_n1@c' (no add); [CREATE] at child entry SV_n2@Ø = SV_before[...] (no
  // add). Only [DEFAULT], [JOIN], [SIGNAL] add the current event.
  if (!key.node || !m_tfg)
    return true;
  auto pred_it = m_static_node_predecessors.find(key);
  if (pred_it == m_static_node_predecessors.end()) {
    return true;
  }
  for (const StaticNodeKey &pred_key : pred_it->second) {
    EdgeKind kind = m_tfg->getEdgeKind(pred_key.node, key.node);
    if (kind == EdgeKind::Control || kind == EdgeKind::Join ||
        kind == EdgeKind::Signal || kind == EdgeKind::Barrier)
      return true;
  }
  return false;
}

bool StaticVectorClockMHP::transfer(const StaticNodeKey &key) {
  if (!key.node)
    return false;

  // Figure 4: merge/Max from predecessors; then add current event only when not
  // Call/Ret/Create target.
  StaticVectorClock incoming = mergePredecessorClocksWithRules(key);

  if (incoming.entries.empty()) {
    if (key.stid < m_static_threads.size()) {
      incoming = initialClockFor(m_static_threads[key.stid]);
    }
  }

  if (shouldAddEventAtNode(key))
    addEventToClock(key, incoming);

  StaticVectorClock &current = m_static_node_clocks[key];
  bool changed = !incoming.leq(current) || !current.leq(incoming);
  if (changed) {
    current = incoming;
  }
  return changed;
}

void StaticVectorClockMHP::computeStaticVectorClocks() {
  if (!m_tfg)
    return;

  bool changed = true;
  while (changed) {
    changed = false;
    for (const StaticNodeKey &key : m_static_node_keys) {
      changed |= transfer(key);
    }
  }
}

bool StaticVectorClockMHP::happensBefore(const StaticVectorClock &lhs,
                                         const StaticVectorClock &rhs) const {
  // Returns true if lhs happens-before rhs (strictly ordered).
  // Paper: use pointwise logic-clock order (svcLeq), not set inclusion.
  return svcLeq(lhs, rhs) && !svcLeq(rhs, lhs);
}

bool StaticVectorClockMHP::happensBefore(const Instruction *i1,
                                         const Instruction *i2) const {
  if (!m_tfg)
    return false;

  if (!i1 || !i2 || i1 == i2)
    return false;

  if (isInstructionThreadAmbiguous(i1) || isInstructionThreadAmbiguous(i2))
    return false;

  auto tid1 = m_inst_to_thread.find(i1);
  auto tid2 = m_inst_to_thread.find(i2);
  if (tid1 == m_inst_to_thread.end() || tid2 == m_inst_to_thread.end()) {
    return false;
  }

  auto keys_it1 = m_inst_to_static_nodes.find(i1);
  auto keys_it2 = m_inst_to_static_nodes.find(i2);
  if (keys_it1 == m_inst_to_static_nodes.end() ||
      keys_it2 == m_inst_to_static_nodes.end() || keys_it1->second.empty() ||
      keys_it2->second.empty()) {
    return false;
  }

  for (const StaticNodeKey &key1 : keys_it1->second) {
    auto it1 = m_static_node_clocks.find(key1);
    if (it1 == m_static_node_clocks.end()) {
      return false;
    }
    for (const StaticNodeKey &key2 : keys_it2->second) {
      auto it2 = m_static_node_clocks.find(key2);
      if (it2 == m_static_node_clocks.end()) {
        return false;
      }
      if (!m_tfg->canReach(key1.node, key2.node)) {
        return false;
      }
      if (!(svcLeq(it1->second, it2->second) &&
            !svcLeq(it2->second, it1->second))) {
        return false;
      }
    }
  }

  return true;
}

void StaticVectorClockMHP::computeMHPPairs() {
  std::vector<const Instruction *> all_insts;
  all_insts.reserve(m_inst_to_thread.size());
  for (Function &func : m_module) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_inst_to_thread.count(inst)) {
        all_insts.push_back(inst);
      }
    }
  }

  for (size_t i = 0; i < all_insts.size(); ++i) {
    for (size_t j = i + 1; j < all_insts.size(); ++j) {
      const Instruction *a = all_insts[i];
      const Instruction *b = all_insts[j];

      ThreadID tid_a = getThreadID(a);
      ThreadID tid_b = getThreadID(b);
      if (tid_a != kUnknownThread && tid_a == tid_b &&
          !m_multi_instance_threads.count(tid_a)) {
        continue;
      }

      if (happensBefore(a, b) || happensBefore(b, a)) {
        continue;
      }

      const Instruction *first = a < b ? a : b;
      const Instruction *second = a < b ? b : a;
      m_mhp_pairs.insert({first, second});
    }
  }
}

bool StaticVectorClockMHP::mayHappenInParallel(const Instruction *i1,
                                               const Instruction *i2) const {
  if (!i1 || !i2 || i1 == i2)
    return false;

  ThreadID tid1 = getThreadID(i1);
  ThreadID tid2 = getThreadID(i2);
  if (tid1 != kUnknownThread && tid1 == tid2 &&
      !m_multi_instance_threads.count(tid1)) {
    return false;
  }

  const Instruction *a = i1 < i2 ? i1 : i2;
  const Instruction *b = i1 < i2 ? i2 : i1;
  if (m_mhp_pairs.count({a, b}))
    return true;

  return !happensBefore(i1, i2) && !happensBefore(i2, i1);
}

bool StaticVectorClockMHP::isPrecomputedMHP(const Instruction *i1,
                                            const Instruction *i2) const {
  if (!i1 || !i2) {
    return false;
  }
  const Instruction *a = i1 < i2 ? i1 : i2;
  const Instruction *b = i1 < i2 ? i2 : i1;
  return m_mhp_pairs.count({a, b}) != 0;
}

InstructionSet
StaticVectorClockMHP::getParallelInstructions(const Instruction *inst) const {
  auto cache_it = m_parallel_instruction_cache.find(inst);
  if (cache_it != m_parallel_instruction_cache.end()) {
    return cache_it->second;
  }

  InstructionSet result;
  for (const auto &pair : m_mhp_pairs) {
    if (pair.first == inst) {
      result.insert(pair.second);
    } else if (pair.second == inst) {
      result.insert(pair.first);
    }
  }
  if (inst) {
    m_parallel_instruction_cache[inst] = result;
  }
  return result;
}

ThreadID StaticVectorClockMHP::getThreadID(const Instruction *inst) const {
  auto it = m_inst_to_thread.find(inst);
  return it != m_inst_to_thread.end() ? it->second : kUnknownThread;
}

bool StaticVectorClockMHP::isMultiInstanceThread(ThreadID tid) const {
  return m_multi_instance_threads.count(tid) != 0;
}

InstructionSet
StaticVectorClockMHP::getInstructionsInThread(ThreadID tid) const {
  auto cache_it = m_thread_instruction_cache.find(tid);
  if (cache_it != m_thread_instruction_cache.end()) {
    return cache_it->second;
  }

  InstructionSet result;
  for (const auto &entry : m_inst_to_thread) {
    if (entry.second == tid) {
      result.insert(entry.first);
    }
  }
  m_thread_instruction_cache[tid] = result;
  return result;
}

void StaticVectorClockMHP::printStatistics(raw_ostream &os) const {
  size_t num_static_threads = m_static_threads.size();
  size_t num_nodes = m_static_node_clocks.size();

  os << "SVC-MHP Statistics:\n";
  os << "===================\n";
  os << "Static Threads: " << num_static_threads << "\n";
  os << "TFG Nodes:      " << num_nodes << "\n";
  os << "MHP Pairs:      " << m_mhp_pairs.size() << "\n";
}

void StaticVectorClockMHP::printResults(raw_ostream &os) const {
  printStatistics(os);
  os << "\nSample MHP pairs (up to 10):\n";
  int shown = 0;
  for (const auto &p : m_mhp_pairs) {
    os << "MHP: ";
    p.first->print(os);
    os << " || ";
    p.second->print(os);
    os << "\n";
    if (++shown >= 10) {
      if (m_mhp_pairs.size() > 10) {
        os << "... (" << (m_mhp_pairs.size() - 10) << " more)\n";
      }
      break;
    }
  }
}

// === Thread-flow graph construction (self contained, no MHPAnalysis) ========

void StaticVectorClockMHP::buildThreadFlowGraph() {
  Function *main_func = m_module.getFunction("main");
  if (!main_func) {
    errs() << "SVC-MHP: no main function found\n";
    return;
  }

  if (m_join_target_analysis) {
    m_join_target_analysis->analyze();
  }

  m_tfg->addThread(0, main_func);
  processFunction(main_func, 0, 0);
  wireSynchronizationEdges();
}

void StaticVectorClockMHP::processFunction(const Function *func, ThreadID tid,
                                           CallContextID ctx) {
  if (!func || func->isDeclaration())
    return;

  auto &active_stack = m_active_call_stack_by_thread[tid];
  if (active_stack.size() >= kCallContextLimit ||
      std::find(active_stack.begin(), active_stack.end(), func) !=
          active_stack.end()) {
    return;
  }

  auto &visited = m_visited_functions_by_thread[tid][ctx];
  if (!visited.insert(func).second)
    return;
  active_stack.push_back(func);

  // --- Pass 1: Create all nodes for this function ---
  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      mapInstructionToThread(&inst, tid);
      SyncNodeType node_type = SyncNodeType::REGULAR_INST;

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
        if (type == ThreadAPI::TD_BAR_INIT && cb->arg_size() >= 3) {
          if (const Value *barrier = m_thread_api->getBarrierVal(&inst)) {
            if (const auto *count =
                    dyn_cast<ConstantInt>(cb->getArgOperand(2))) {
              m_barrier_expected_counts[barrier->stripPointerCasts()] =
                  static_cast<size_t>(count->getZExtValue());
            }
          }
        }
        if (m_thread_api->isTDFork(&inst)) {
          node_type = SyncNodeType::THREAD_FORK;
        } else if (m_thread_api->isTDJoin(&inst)) {
          node_type = SyncNodeType::THREAD_JOIN;
        } else if (m_thread_api->isTDAcquire(&inst)) {
          node_type = SyncNodeType::LOCK_ACQUIRE;
        } else if (m_thread_api->isTDRelease(&inst)) {
          node_type = SyncNodeType::LOCK_RELEASE;
        } else if (m_thread_api->isTDExit(&inst)) {
          node_type = SyncNodeType::THREAD_EXIT;
        } else if (m_thread_api->isTDCondWait(&inst)) {
          node_type = SyncNodeType::COND_WAIT;
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          node_type = SyncNodeType::COND_SIGNAL;
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          node_type = SyncNodeType::COND_BROADCAST;
        } else if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT ||
                   type == ThreadAPI::TD_BARRIER_ARRIVE ||
                   m_thread_api->isTDBarWait(&inst)) {
          node_type = SyncNodeType::BARRIER_WAIT;
        }
      }
      m_tfg->createNode(&inst, node_type, tid, ctx);
    }
  }

  // --- Pass 2: Add edges and handle synchronization logic ---
  SyncNode *entry_node = nullptr;
  if (!func->empty() && !func->front().empty()) {
    entry_node = m_tfg->getNode(&func->front().front(), tid, ctx);
    if (entry_node && ctx == 0) {
      m_tfg->setThreadEntryNode(tid, entry_node);
    }
  }

  for (const BasicBlock &bb : *func) {
    for (const Instruction &inst : bb) {
      SyncNode *node = m_tfg->getNode(&inst, tid, ctx);
      if (!node) {
        continue;
      }

      if (&inst != &bb.front()) {
        const Instruction *prev_inst = inst.getPrevNode();
        if (prev_inst) {
          SyncNode *prev_node = m_tfg->getNode(prev_inst, tid, ctx);
          if (prev_node) {
            m_tfg->addIntraThreadEdge(prev_node, node);
          }
        }
      }

      if (inst.isTerminator()) {
        for (const BasicBlock *succ : successors(inst.getParent())) {
          if (!succ->empty()) {
            SyncNode *succ_node = m_tfg->getNode(&succ->front(), tid, ctx);
            if (succ_node) {
              m_tfg->addIntraThreadEdge(node, succ_node);
            }
          }
        }
      }

      if (const CallBase *cb = dyn_cast<CallBase>(&inst)) {
        ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
        if (m_thread_api->isTDFork(&inst)) {
          handleThreadFork(&inst, node, tid);
        } else if (m_thread_api->isTDJoin(&inst)) {
          handleThreadJoin(&inst, node, tid);
        } else if (m_thread_api->isTDAcquire(&inst)) {
          handleLockAcquire(&inst, node);
        } else if (m_thread_api->isTDRelease(&inst)) {
          handleLockRelease(&inst, node);
        } else if (m_thread_api->isTDCondWait(&inst)) {
          handleCondWait(&inst, node);
        } else if (m_thread_api->isTDCondSignal(&inst)) {
          handleCondSignal(&inst, node);
        } else if (m_thread_api->isTDCondBroadcast(&inst)) {
          handleCondSignal(&inst, node);
        } else if (type == ThreadAPI::TD_LATCH_ARRIVE_WAIT ||
                   type == ThreadAPI::TD_BARRIER_ARRIVE ||
                   m_thread_api->isTDBarWait(&inst)) {
          handleBarrier(&inst, node);
        } else if (type == ThreadAPI::TD_DETACH) {
          handleThreadDetach(&inst);
        } else {
          auto processCallee = [&](const Function *callee) {
            if (!callee || callee->isDeclaration())
              return;

            CallContextID callee_ctx = node->getNodeID();
            processFunction(callee, tid, callee_ctx);
            SyncNode *callee_entry =
                m_tfg->getNode(&callee->front().front(), tid, callee_ctx);
            if (callee_entry) {
              m_tfg->addCallEdge(node, callee_entry);
            }
            std::vector<SyncNode *> callee_exits =
                m_tfg->getFunctionExitNodes(tid, callee, callee_ctx);
            if (callee_exits.empty()) {
              return;
            }

            const Instruction *next_inst = inst.getNextNode();
            if (next_inst) {
              SyncNode *return_site = m_tfg->getNode(next_inst, tid, ctx);
              if (return_site) {
                for (SyncNode *callee_exit : callee_exits) {
                  m_tfg->addRetEdge(callee_exit, return_site);
                }
                m_ret_to_call[return_site] = node;
              }
            } else if (inst.isTerminator()) {
              for (const BasicBlock *succ : successors(inst.getParent())) {
                if (succ->empty()) {
                  continue;
                }
                SyncNode *return_site =
                    m_tfg->getNode(&succ->front(), tid, ctx);
                if (return_site) {
                  for (SyncNode *callee_exit : callee_exits) {
                    m_tfg->addRetEdge(callee_exit, return_site);
                  }
                  m_ret_to_call[return_site] = node;
                }
              }
            }
          };

          if (const Function *direct = m_thread_api->getCallee(cb)) {
            processCallee(direct);
          } else if (m_call_graph) {
            bool resolved_indirect_target = false;
            bool has_unresolved_indirect_target = false;
            if (CallGraphNode *cgNode = (*m_call_graph)[cb->getFunction()]) {
              for (auto &callRecord : *cgNode) {
                if (!callRecord.first.hasValue() ||
                    dyn_cast_or_null<CallBase>(*callRecord.first) != cb) {
                  continue;
                }
                CallGraphNode *calleeNode = callRecord.second;
                if (!calleeNode) {
                  has_unresolved_indirect_target = true;
                  continue;
                }
                Function *callee = calleeNode->getFunction();
                if (!callee) {
                  has_unresolved_indirect_target = true;
                  continue;
                }
                if (callee->isDeclaration()) {
                  has_unresolved_indirect_target = true;
                  continue;
                }
                resolved_indirect_target = true;
                processCallee(callee);
              }
            }
            if (!resolved_indirect_target || has_unresolved_indirect_target) {
              enableIndirectForkConservatism();
            }
          } else {
            enableIndirectForkConservatism();
          }
        }
      }

      if (isa<ReturnInst>(inst) || m_thread_api->isTDExit(&inst)) {
        m_tfg->setFunctionExitNode(tid, func, node, ctx);
        if (ctx == 0 && m_tfg->getThreadEntry(tid) == func) {
          m_tfg->setThreadExitNode(tid, node);
        }
      }
    }
  }

  active_stack.pop_back();
}

void StaticVectorClockMHP::mapInstructionToThread(const Instruction *inst,
                                                  ThreadID tid) {
  if (!inst) {
    return;
  }

  if (m_has_unresolved_fork &&
      m_thread_entry_candidates.find(inst->getFunction()) !=
          m_thread_entry_candidates.end()) {
    m_inst_to_thread[inst] = kUnknownThread;
    return;
  }

  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    m_inst_to_thread[inst] = tid;
    return;
  }

  if (it->second == kUnknownThread || it->second == tid) {
    return;
  }

  it->second = kUnknownThread;
}

bool StaticVectorClockMHP::isInstructionThreadAmbiguous(
    const Instruction *inst) const {
  if (!inst) {
    return true;
  }
  auto static_it = m_inst_to_static_nodes.find(inst);
  if (static_it == m_inst_to_static_nodes.end() || static_it->second.empty()) {
    return true;
  }
  auto it = m_inst_to_thread.find(inst);
  if (it == m_inst_to_thread.end()) {
    return true;
  }
  return it->second == kUnknownThread;
}

bool StaticVectorClockMHP::isMustIntraThreadEdge(const SyncNode *from,
                                                 const SyncNode *to) const {
  if (!from || !to) {
    return false;
  }
  if (from->getThreadID() != to->getThreadID()) {
    return false;
  }

  const Instruction *from_inst = from->getInstruction();
  const Instruction *to_inst = to->getInstruction();
  if (!from_inst || !to_inst) {
    return false;
  }

  if (from_inst->getParent() == to_inst->getParent()) {
    return from_inst->getNextNode() == to_inst;
  }

  const Function *func = from_inst->getFunction();
  if (!func || func != to_inst->getFunction()) {
    return false;
  }

  const PostDominatorTree &PDT = getPostDomTree(func);
  return PDT.dominates(to_inst->getParent(), from_inst->getParent());
}

const PostDominatorTree &
StaticVectorClockMHP::getPostDomTree(const Function *func) const {
  auto it = m_post_dom_cache.find(func);
  if (it != m_post_dom_cache.end()) {
    return *(it->second);
  }
  auto PDT = std::make_unique<PostDominatorTree>();
  PDT->recalculate(*const_cast<Function *>(func));
  auto *pdtPtr = PDT.get();
  m_post_dom_cache[func] = std::move(PDT);
  return *pdtPtr;
}

void StaticVectorClockMHP::enableIndirectForkConservatism() {
  if (m_has_unresolved_fork) {
    return;
  }
  m_has_unresolved_fork = true;

  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    if (!func.hasAddressTaken() || !isLikelyThreadEntryCandidate(func)) {
      continue;
    }

    m_thread_entry_candidates.insert(&func);
    for (Instruction &inst : instructions(func)) {
      auto it = m_inst_to_thread.find(&inst);
      if (it == m_inst_to_thread.end()) {
        m_inst_to_thread[&inst] = kUnknownThread;
      } else if (it->second != kUnknownThread) {
        it->second = kUnknownThread;
      }
    }
  }
}

ThreadID StaticVectorClockMHP::allocateThreadID() { return m_next_thread_id++; }

void StaticVectorClockMHP::handleThreadFork(const Instruction *fork_inst,
                                            SyncNode *node,
                                            ThreadID parent_tid) {
  ThreadID new_tid = allocateThreadID();

  const bool multi_instance =
      (fork_inst && m_thread_multiplicity &&
       m_thread_multiplicity->instructionMayExecuteMultipleTimes(fork_inst)) ||
      isMultiInstanceThread(parent_tid);
  if (multi_instance) {
    m_multi_instance_threads.insert(new_tid);
  }

  node->setForkedThread(new_tid);

  m_thread_fork_sites[new_tid] = fork_inst;
  m_thread_parents[new_tid] = parent_tid;
  m_thread_children[parent_tid].push_back(new_tid);
  m_fork_to_thread[fork_inst] = new_tid;

  const Value *pthread_ptr = m_thread_api->getForkedThread(fork_inst);
  if (pthread_ptr) {
    m_pthread_value_to_threads[pthread_ptr].insert(new_tid);
    m_thread_to_pthread_value[new_tid] = pthread_ptr;
  }

  const Value *forked_fun_val = m_thread_api->getForkedFun(fork_inst);
  if (const Function *forked_fun = dyn_cast_or_null<Function>(forked_fun_val)) {
    m_tfg->addThread(new_tid, forked_fun);
    processFunction(forked_fun, new_tid, 0);
    if (SyncNode *child_entry = m_tfg->getThreadEntryNode(new_tid)) {
      m_tfg->addInterThreadEdge(node, child_entry);
    }
  } else {
    enableIndirectForkConservatism();
    m_multi_instance_threads.insert(new_tid);
  }
}

void StaticVectorClockMHP::handleThreadJoin(const Instruction *join_inst,
                                            SyncNode *node,
                                            ThreadID parent_tid) {
  const Value *joined_thread_val = m_thread_api->getJoinedThread(join_inst);
  ThreadID joined_tid = 0;
  bool found = false;
  std::unordered_set<const Value *> joined_roots;

  if (joined_thread_val) {
    JoinTargetAnalysis::traceThreadHandleRoots(joined_thread_val, &m_module,
                                               joined_roots);
    const Value *root =
        JoinTargetAnalysis::traceThreadHandleRoot(joined_thread_val, &m_module);
    auto it = m_pthread_value_to_threads.find(root ? root : joined_thread_val);
    if (it != m_pthread_value_to_threads.end() && it->second.size() == 1 &&
        !isMultiInstanceThread(*it->second.begin()) &&
        !m_detached_threads.count(*it->second.begin())) {
      joined_tid = *it->second.begin();
      found = true;
    }
  }

  if (!found && joined_roots.size() <= 1 && m_join_target_analysis) {
    if (m_join_target_analysis->isUnambiguousJoin(join_inst)) {
      std::vector<const Instruction *> possible_forks =
          m_join_target_analysis->getFeasibleJoinedForks(join_inst);
      if (possible_forks.size() == 1) {
        auto it = m_fork_to_thread.find(possible_forks.front());
        if (it != m_fork_to_thread.end() &&
            !isMultiInstanceThread(it->second) &&
            !m_detached_threads.count(it->second)) {
          joined_tid = it->second;
          found = true;
        }
      }
    }
  }

  auto add_join_edge = [&](ThreadID tid) {
    std::vector<SyncNode *> child_exits = m_tfg->getThreadExitNodes(tid);
    if (!child_exits.empty()) {
      for (SyncNode *child_exit : child_exits) {
        m_tfg->addInterThreadEdge(child_exit, node, EdgeKind::Join);
      }
      node->setJoinedThread(tid);
      m_join_to_thread[join_inst] = tid;
    }
  };

  if (found && joined_tid != 0) {
    add_join_edge(joined_tid);
  } else {
    (void)parent_tid;
  }
}

void StaticVectorClockMHP::handleThreadDetach(const Instruction *detach_inst) {
  const auto *call = dyn_cast<CallBase>(detach_inst);
  if (!call || call->arg_size() < 1) {
    return;
  }

  const Value *detached_thread_val = call->getArgOperand(0);
  std::unordered_set<const Value *> detached_roots;
  JoinTargetAnalysis::traceThreadHandleRoots(detached_thread_val, &m_module,
                                             detached_roots);
  if (detached_roots.empty()) {
    if (const Value *root = JoinTargetAnalysis::traceThreadHandleRoot(
            detached_thread_val, &m_module)) {
      detached_roots.insert(root);
    } else if (detached_thread_val) {
      detached_roots.insert(detached_thread_val->stripPointerCasts());
    }
  }

  for (const Value *root : detached_roots) {
    auto it = m_pthread_value_to_threads.find(root);
    if (it == m_pthread_value_to_threads.end()) {
      continue;
    }
    m_detached_threads.insert(it->second.begin(), it->second.end());
  }
}

void StaticVectorClockMHP::handleLockAcquire(const Instruction *lock_inst,
                                             SyncNode *node) {
  const Value *lock = m_thread_api->getAnalysisLockIdentity(lock_inst);
  node->setLockValue(lock);
}

void StaticVectorClockMHP::handleLockRelease(const Instruction *unlock_inst,
                                             SyncNode *node) {
  const Value *lock = m_thread_api->getAnalysisLockIdentity(unlock_inst);
  node->setLockValue(lock);
}

void StaticVectorClockMHP::handleCondWait(const Instruction *wait_inst,
                                          SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(wait_inst);
  const Value *mutex = m_thread_api->getCondMutex(wait_inst);
  node->setCondValue(cond);
  node->setLockValue(mutex);
  m_condvar_waits[cond].push_back(node);
}

void StaticVectorClockMHP::handleCondSignal(const Instruction *signal_inst,
                                            SyncNode *node) {
  const Value *cond = m_thread_api->getCondVal(signal_inst);
  node->setCondValue(cond);
  m_condvar_signals[cond].push_back(node);
}

void StaticVectorClockMHP::handleBarrier(const Instruction *barrier_inst,
                                         SyncNode *node) {
  const Value *barrier = m_thread_api->getBarrierVal(barrier_inst);
  if (!barrier) {
    barrier = barrier_inst;
  }
  node->setLockValue(barrier);

  BarrierParticipant current;
  current.arrival = node;
  const auto *call = dyn_cast<CallBase>(barrier_inst);
  const ThreadAPI::TD_TYPE type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (type != ThreadAPI::TD_BARRIER_ARRIVE) {
    current.continuations = getBarrierContinuations(barrier_inst);
  }
  auto &next_phase_by_thread = m_barrier_phase_by_thread[barrier];
  auto &pending_phase_by_thread =
      m_pending_split_barrier_phase_by_thread[barrier];
  size_t &next_phase = next_phase_by_thread[node->getThreadID()];
  size_t phase = next_phase;

  if (type == ThreadAPI::TD_BARRIER_ARRIVE) {
    pending_phase_by_thread[node->getThreadID()] = phase;
  } else if (type == ThreadAPI::TD_BARRIER_WAIT_CPP20) {
    auto pending_it = pending_phase_by_thread.find(node->getThreadID());
    if (pending_it != pending_phase_by_thread.end()) {
      phase = pending_it->second;
      pending_phase_by_thread.erase(pending_it);
      next_phase = std::max(next_phase, phase + 1);
    } else {
      phase = next_phase++;
    }
  } else {
    pending_phase_by_thread.erase(node->getThreadID());
    phase = next_phase++;
  }
  auto &participants = m_barrier_waits[barrier][phase];
  std::unordered_set<ThreadID> distinct_threads;
  for (const BarrierParticipant &previous : participants) {
    if (previous.arrival) {
      distinct_threads.insert(previous.arrival->getThreadID());
    }
  }
  distinct_threads.insert(node->getThreadID());
  const Value *barrier_key = barrier ? barrier->stripPointerCasts() : nullptr;
  const size_t expected_count =
      barrier_key && m_barrier_expected_counts.count(barrier_key)
          ? m_barrier_expected_counts.at(barrier_key)
          : 0;
  const bool phase_complete =
      expected_count == 0 || distinct_threads.size() >= expected_count;
  for (const BarrierParticipant &previous : participants) {
    if (!previous.arrival ||
        previous.arrival->getThreadID() == node->getThreadID()) {
      continue;
    }

    if (phase_complete) {
      for (SyncNode *cont : current.continuations) {
        if (cont) {
          m_tfg->addInterThreadEdge(previous.arrival, cont, EdgeKind::Barrier);
        }
      }
      for (SyncNode *cont : previous.continuations) {
        if (cont) {
          m_tfg->addInterThreadEdge(node, cont, EdgeKind::Barrier);
        }
      }
    }
  }
  participants.push_back(std::move(current));
}

void StaticVectorClockMHP::wireSynchronizationEdges() {
  if (!m_tfg)
    return;

  errs() << "Wiring synchronization edges for SVC-MHP...\n";
  // Barrier edges are added in handleBarrier only when the phase is complete.
  // Rewiring all recorded participants here would over-order incomplete phases.
  //
  // Condition variable and atomic synchronization stay in HappensBeforeAnalysis.
  // Adding unconditional TFG edges here would be stronger than the witness-
  // based HB model and can suppress real MHP/race candidates.
}

std::vector<SyncNode *> StaticVectorClockMHP::getBarrierContinuations(
    const Instruction *barrier_inst) const {
  std::vector<SyncNode *> continuations;
  if (!barrier_inst) {
    return continuations;
  }

  bool stopped_at_next_wait = false;
  if (const Instruction *next = barrier_inst->getNextNode()) {
    if (m_thread_api->isTDBarWait(next)) {
      stopped_at_next_wait = true;
    } else {
      for (SyncNode *next_node :
           m_tfg->getNodes(next, m_inst_to_thread.at(barrier_inst))) {
        continuations.push_back(next_node);
      }
    }
  }

  if (barrier_inst->isTerminator()) {
    for (const BasicBlock *succ : successors(barrier_inst->getParent())) {
      if (succ->empty()) {
        continue;
      }
      if (m_thread_api->isTDBarWait(&succ->front())) {
        stopped_at_next_wait = true;
        continue;
      }
      for (SyncNode *succ_node :
           m_tfg->getNodes(&succ->front(), m_inst_to_thread.at(barrier_inst))) {
        continuations.push_back(succ_node);
      }
    }
  }

  if (continuations.empty() && !stopped_at_next_wait) {
    for (SyncNode *self :
         m_tfg->getNodes(barrier_inst, m_inst_to_thread.at(barrier_inst))) {
      continuations.push_back(self);
    }
  }

  return continuations;
}

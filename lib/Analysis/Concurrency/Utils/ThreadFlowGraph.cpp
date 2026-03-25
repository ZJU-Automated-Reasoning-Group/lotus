/**
 * @file ThreadFlowGraph.cpp
 * @brief Implementation of Thread Flow Graph classes
 *
 * The Thread Flow Graph (TFG) is a graph representation of the concurrent
 * program. Nodes represent synchronization events or instructions. Edges
 * represent:
 * 1. Intra-thread control flow (program order)
 * 2. Inter-thread synchronization (fork, join, signal, etc.)
 */

#include "Analysis/Concurrency/Utils/ThreadFlowGraph.h"

#include <functional>
#include <queue>
#include <unordered_set>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace {

bool bfsReachableSameThread(const SyncNode *from, const SyncNode *to) {
  if (!from || !to || from == to) {
    return false;
  }

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;
  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    for (SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() != from->getThreadID()) {
        continue;
      }
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

} // namespace

// ============================================================================
// SyncNode Implementation
// ============================================================================

size_t SyncNode::next_id = 0;

void SyncNode::print(raw_ostream &os) const {
  os << "SyncNode[" << m_node_id << "]: ";
  os << "Type=" << getSyncNodeTypeName(m_type);
  os << ", Thread=" << m_thread_id;
  if (m_call_context_id != 0) {
    os << ", Ctx=" << m_call_context_id;
  }

  if (m_instruction) {
    os << ", Inst=";
    m_instruction->print(os);
  }

  if (m_lock_value) {
    os << ", Lock=";
    m_lock_value->printAsOperand(os, false);
  }

  if (m_forked_thread != 0) {
    os << ", ForkedThread=" << m_forked_thread;
  }

  if (m_joined_thread != 0) {
    os << ", JoinedThread=" << m_joined_thread;
  }
}

std::string SyncNode::toString() const {
  std::string str;
  raw_string_ostream os(str);
  print(os);
  return os.str();
}

// ============================================================================
// ThreadFlowGraph Implementation
// ============================================================================

ThreadFlowGraph::~ThreadFlowGraph() {
  for (auto *node : m_all_nodes) {
    delete node;
  }
}

SyncNode *ThreadFlowGraph::createNode(const Instruction *inst,
                                      SyncNodeType type, ThreadID tid,
                                      CallContextID ctx) {
  if (inst) {
    InstThreadKey key{inst, tid, ctx};
    auto it = m_inst_thread_to_node.find(key);
    if (it != m_inst_thread_to_node.end()) {
      return it->second;
    }
  }
  auto *node = new SyncNode(inst, type, tid, ctx);
  m_all_nodes.push_back(node);

  if (inst) {
    InstThreadKey key{inst, tid, ctx};
    m_inst_thread_to_node[key] = node;
    m_inst_to_nodes[inst].push_back(node);
  }

  return node;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst,
                                   ThreadID tid,
                                   CallContextID ctx) const {
  if (!inst) {
    return nullptr;
  }
  auto it = m_inst_thread_to_node.find({inst, tid, ctx});
  return it != m_inst_thread_to_node.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst,
                                   ThreadID tid) const {
  if (!inst) {
    return nullptr;
  }
  SyncNode *match = nullptr;
  for (SyncNode *node : getNodes(inst, tid)) {
    if (match) {
      return nullptr;
    }
    match = node;
  }
  return match;
}

SyncNode *ThreadFlowGraph::getNode(const Instruction *inst) const {
  auto it = m_inst_to_nodes.find(inst);
  if (it != m_inst_to_nodes.end() && it->second.size() == 1) {
    return it->second.front();
  }
  return nullptr;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodes(const Instruction *inst) const {
  auto it = m_inst_to_nodes.find(inst);
  if (it == m_inst_to_nodes.end()) {
    return {};
  }
  return it->second;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodes(const Instruction *inst,
                                                  ThreadID tid) const {
  std::vector<SyncNode *> result;
  auto it = m_inst_to_nodes.find(inst);
  if (it == m_inst_to_nodes.end()) {
    return result;
  }
  for (SyncNode *node : it->second) {
    if (node->getThreadID() == tid) {
      result.push_back(node);
    }
  }
  return result;
}

void ThreadFlowGraph::addThread(ThreadID tid, const Function *entry) {
  m_thread_entries[tid] = entry;
}

const Function *ThreadFlowGraph::getThreadEntry(ThreadID tid) const {
  auto it = m_thread_entries.find(tid);
  return it != m_thread_entries.end() ? it->second : nullptr;
}

std::vector<ThreadID> ThreadFlowGraph::getAllThreads() const {
  std::vector<ThreadID> threads;
  threads.reserve(m_thread_entries.size());
  for (const auto &pair : m_thread_entries) {
    threads.push_back(pair.first);
  }
  return threads;
}

void ThreadFlowGraph::setThreadEntryNode(ThreadID tid, SyncNode *entry) {
  m_thread_entry_nodes[tid] = entry;
}

void ThreadFlowGraph::setThreadExitNode(ThreadID tid, SyncNode *exit) {
  if (!exit) {
    return;
  }
  std::vector<SyncNode *> &exits = m_thread_exit_nodes[tid];
  if (std::find(exits.begin(), exits.end(), exit) == exits.end()) {
    exits.push_back(exit);
  }
}

SyncNode *ThreadFlowGraph::getThreadEntryNode(ThreadID tid) const {
  auto it = m_thread_entry_nodes.find(tid);
  return it != m_thread_entry_nodes.end() ? it->second : nullptr;
}

SyncNode *ThreadFlowGraph::getThreadExitNode(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  if (it == m_thread_exit_nodes.end() || it->second.size() != 1) {
    return nullptr;
  }
  return it->second.front();
}

std::vector<SyncNode *> ThreadFlowGraph::getThreadExitNodes(ThreadID tid) const {
  auto it = m_thread_exit_nodes.find(tid);
  return it != m_thread_exit_nodes.end() ? it->second : std::vector<SyncNode *>();
}

void ThreadFlowGraph::addIntraThreadEdge(SyncNode *from, SyncNode *to) {
  if (from && to) {
    from->addSuccessor(to);
    to->addPredecessor(from);
    m_edge_kinds[{const_cast<const SyncNode *>(from),
                  const_cast<const SyncNode *>(to)}] = EdgeKind::Control;
  }
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to) {
  addInterThreadEdge(from, to, EdgeKind::Create);
}

void ThreadFlowGraph::addInterThreadEdge(SyncNode *from, SyncNode *to,
                                         EdgeKind kind) {
  if (from && to) {
    from->addSuccessor(to);
    to->addPredecessor(from);
    m_edge_kinds[{const_cast<const SyncNode *>(from),
                  const_cast<const SyncNode *>(to)}] = kind;
  }
}

void ThreadFlowGraph::addCallEdge(SyncNode *call_site, SyncNode *callee_entry) {
  if (call_site && callee_entry) {
    call_site->addSuccessor(callee_entry);
    callee_entry->addPredecessor(call_site);
    m_edge_kinds[{const_cast<const SyncNode *>(call_site),
                  const_cast<const SyncNode *>(callee_entry)}] = EdgeKind::Call;
  }
}

void ThreadFlowGraph::addRetEdge(SyncNode *callee_exit, SyncNode *return_site) {
  if (callee_exit && return_site) {
    callee_exit->addSuccessor(return_site);
    return_site->addPredecessor(callee_exit);
    m_edge_kinds[{const_cast<const SyncNode *>(callee_exit),
                  const_cast<const SyncNode *>(return_site)}] = EdgeKind::Ret;
  }
}

EdgeKind ThreadFlowGraph::getEdgeKind(const SyncNode *from,
                                      const SyncNode *to) const {
  auto it = m_edge_kinds.find({from, to});
  return it != m_edge_kinds.end() ? it->second : EdgeKind::Control;
}

void ThreadFlowGraph::setFunctionExitNode(ThreadID tid,
                                          const llvm::Function *func,
                                          SyncNode *exit_node,
                                          CallContextID ctx) {
  if (!exit_node) {
    return;
  }
  std::vector<SyncNode *> &exits = m_func_exit_nodes[{tid, func, ctx}];
  if (std::find(exits.begin(), exits.end(), exit_node) == exits.end()) {
    exits.push_back(exit_node);
  }
}

SyncNode *
ThreadFlowGraph::getFunctionExitNode(ThreadID tid,
                                     const llvm::Function *func,
                                     CallContextID ctx) const {
  auto it = m_func_exit_nodes.find({tid, func, ctx});
  if (it == m_func_exit_nodes.end() || it->second.size() != 1) {
    return nullptr;
  }
  return it->second.front();
}

std::vector<SyncNode *>
ThreadFlowGraph::getFunctionExitNodes(ThreadID tid,
                                      const llvm::Function *func,
                                      CallContextID ctx) const {
  auto it = m_func_exit_nodes.find({tid, func, ctx});
  return it != m_func_exit_nodes.end() ? it->second : std::vector<SyncNode *>();
}

std::vector<SyncNode *>
ThreadFlowGraph::getNodesOfType(SyncNodeType type) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getType() == type) {
      result.push_back(node);
    }
  }
  return result;
}

std::vector<SyncNode *> ThreadFlowGraph::getNodesInThread(ThreadID tid) const {
  std::vector<SyncNode *> result;
  for (auto *node : m_all_nodes) {
    if (node->getThreadID() == tid) {
      result.push_back(node);
    }
  }
  return result;
}

void ThreadFlowGraph::print(raw_ostream &os) const {
  os << "Thread Flow Graph:\n";
  os << "==================\n";
  os << "Total Nodes: " << m_all_nodes.size() << "\n";
  os << "Total Threads: " << m_thread_entries.size() << "\n\n";

  for (auto *node : m_all_nodes) {
    node->print(os);
    os << "\n";

    if (!node->getSuccessors().empty()) {
      os << "  Successors: ";
      for (auto *succ : node->getSuccessors()) {
        os << succ->getNodeID() << " ";
      }
      os << "\n";
    }
  }
}

void ThreadFlowGraph::printAsDot(raw_ostream &os) const {
  os << "digraph ThreadFlowGraph {\n";
  os << "  rankdir=TB;\n";
  os << "  node [shape=box];\n\n";

  // Define nodes
  for (auto *node : m_all_nodes) {
    os << "  node" << node->getNodeID() << " [label=\"";
    os << "ID:" << node->getNodeID() << "\\n";
    os << "T:" << node->getThreadID() << "\\n";
    os << getSyncNodeTypeName(node->getType());
    os << "\"];\n";
  }

  os << "\n";

  // Define edges
  for (auto *node : m_all_nodes) {
    for (auto *succ : node->getSuccessors()) {
      os << "  node" << node->getNodeID() << " -> node" << succ->getNodeID();

      // Different colors for different edge types
      if (node->getThreadID() != succ->getThreadID()) {
        os << " [color=red, style=dashed]"; // Inter-thread edge
      } else if (isSynchronizationNode(node->getType())) {
        os << " [color=blue]"; // Synchronization edge
      }

      os << ";\n";
    }
  }

  os << "}\n";
}

void ThreadFlowGraph::dumpToFile(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  printAsDot(file);
  file.close();
}

// ============================================================================
// Reachability Index Implementation
// ============================================================================

void ThreadFlowGraph::buildReachabilityIndex() {
  if (m_index_built) {
    return;
  }

  errs() << "Building TFG reachability index...\n";

  buildReverseEdges();

  for (const auto &entry : m_thread_entries) {
    ThreadID tid = entry.first;
    buildTopologicalOrder(tid);
    buildSCCs(tid);
  }

  m_index_built = true;

  size_t total_indexed = 0;
  for (const auto &entry : m_topo_nodes) {
    total_indexed += entry.second.size();
  }
  errs() << "Indexed " << total_indexed << " nodes across "
         << m_topo_nodes.size() << " threads\n";
}

void ThreadFlowGraph::buildReverseEdges() {
  m_reverse_edges.clear();
  for (SyncNode *node : m_all_nodes) {
    for (SyncNode *succ : node->getSuccessors()) {
      m_reverse_edges[succ].push_back(node);
    }
  }
}

void ThreadFlowGraph::buildTopologicalOrder(ThreadID tid) {
  std::vector<SyncNode *> &order = m_topo_nodes[tid];
  order.clear();

  std::vector<SyncNode *> thread_nodes;
  for (SyncNode *node : m_all_nodes) {
    if (node->getThreadID() == tid) {
      thread_nodes.push_back(node);
    }
  }

  if (thread_nodes.empty()) {
    return;
  }

  std::unordered_map<SyncNode *, int> in_degree;
  std::unordered_map<SyncNode *, std::vector<SyncNode *>> adj;

  for (SyncNode *node : thread_nodes) {
    in_degree[node] = 0;
  }

  for (SyncNode *node : thread_nodes) {
    for (SyncNode *succ : node->getSuccessors()) {
      if (succ->getThreadID() == tid) {
        adj[node].push_back(succ);
        in_degree[succ]++;
      }
    }
  }

  std::queue<SyncNode *> q;
  for (SyncNode *node : thread_nodes) {
    if (in_degree[node] == 0) {
      q.push(node);
    }
  }

  while (!q.empty()) {
    SyncNode *node = q.front();
    q.pop();

    int order_num = static_cast<int>(order.size());
    m_topo_order[node->getNodeID()] = order_num;
    order.push_back(node);

    for (SyncNode *succ : adj[node]) {
      in_degree[succ]--;
      if (in_degree[succ] == 0) {
        q.push(succ);
      }
    }
  }

  if (order.size() != thread_nodes.size()) {
    for (SyncNode *node : thread_nodes) {
      if (m_topo_order.find(node->getNodeID()) == m_topo_order.end()) {
        int order_num = static_cast<int>(order.size());
        m_topo_order[node->getNodeID()] = order_num;
        order.push_back(node);
      }
    }
  }
}

void ThreadFlowGraph::buildSCCs(ThreadID tid) {
  std::unordered_map<SyncNode *, int> index;
  std::unordered_map<SyncNode *, int> lowlink;
  std::unordered_map<SyncNode *, bool> on_stack;
  std::vector<SyncNode *> stack;
  int current_index = 0;

  std::function<void(SyncNode *)> strongConnect = [&](SyncNode *v) {
    index[v] = current_index;
    lowlink[v] = current_index;
    current_index++;
    stack.push_back(v);
    on_stack[v] = true;

    for (SyncNode *w : v->getSuccessors()) {
      if (w->getThreadID() != tid)
        continue;

      if (index.find(w) == index.end()) {
        strongConnect(w);
        lowlink[v] = std::min(lowlink[v], lowlink[w]);
      } else if (on_stack[w]) {
        lowlink[v] = std::min(lowlink[v], index[w]);
      }
    }

    if (lowlink[v] == index[v]) {
      SyncNode *representative = v;
      SyncNode *w;
      do {
        w = stack.back();
        stack.pop_back();
        on_stack[w] = false;
        m_scc_representative[w] = representative;
      } while (w != v);
    }
  };

  for (SyncNode *node : m_all_nodes) {
    if (node->getThreadID() == tid && index.find(node) == index.end()) {
      strongConnect(node);
    }
  }
}

bool ThreadFlowGraph::canReach(const SyncNode *from, const SyncNode *to) const {
  if (!from || !to || from == to) {
    return false;
  }

  if (m_index_built && from->getThreadID() == to->getThreadID()) {
    return canReachViaIndex(from, to);
  }

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;

  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    for (SyncNode *succ : current->getSuccessors()) {
      if (visited.insert(succ).second) {
        worklist.push_back(succ);
      }
    }
  }

  return false;
}

bool ThreadFlowGraph::canReachViaIndex(const SyncNode *from,
                                       const SyncNode *to) const {
  auto from_it = m_topo_order.find(from->getNodeID());
  auto to_it = m_topo_order.find(to->getNodeID());

  if (from_it == m_topo_order.end() || to_it == m_topo_order.end()) {
    return bfsReachableSameThread(from, to);
  }

  if (from_it->second > to_it->second) {
    auto from_rep = m_scc_representative.find(from);
    auto to_rep = m_scc_representative.find(to);

    if (from_rep != m_scc_representative.end() &&
        to_rep != m_scc_representative.end() &&
        from_rep->second == to_rep->second) {
      return bfsReachableSameThread(from, to);
    }

    return false;
  }

  std::deque<const SyncNode *> worklist;
  std::unordered_set<const SyncNode *> visited;

  worklist.push_back(from);
  visited.insert(from);

  while (!worklist.empty()) {
    const SyncNode *current = worklist.front();
    worklist.pop_front();

    if (current == to) {
      return true;
    }

    auto current_it = m_topo_order.find(current->getNodeID());
    if (current_it != m_topo_order.end() &&
        current_it->second > to_it->second) {
      continue;
    }

    for (SyncNode *succ : current->getSuccessors()) {
      if (succ->getThreadID() == from->getThreadID()) {
        if (visited.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  }

  return false;
}

int ThreadFlowGraph::getTopologicalOrder(const SyncNode *node) const {
  auto it = m_topo_order.find(node->getNodeID());
  return it != m_topo_order.end() ? it->second : -1;
}

const std::vector<SyncNode *> &
ThreadFlowGraph::getTopologicalOrderNodes(ThreadID tid) const {
  static const std::vector<SyncNode *> empty;
  auto it = m_topo_nodes.find(tid);
  return it != m_topo_nodes.end() ? it->second : empty;
}

// ============================================================================
// Utility Functions
// ============================================================================

namespace mhp {

StringRef getSyncNodeTypeName(SyncNodeType type) {
  switch (type) {
  case SyncNodeType::THREAD_START:
    return "THREAD_START";
  case SyncNodeType::THREAD_FORK:
    return "THREAD_FORK";
  case SyncNodeType::THREAD_JOIN:
    return "THREAD_JOIN";
  case SyncNodeType::THREAD_EXIT:
    return "THREAD_EXIT";
  case SyncNodeType::LOCK_ACQUIRE:
    return "LOCK_ACQUIRE";
  case SyncNodeType::LOCK_RELEASE:
    return "LOCK_RELEASE";
  case SyncNodeType::COND_WAIT:
    return "COND_WAIT";
  case SyncNodeType::COND_SIGNAL:
    return "COND_SIGNAL";
  case SyncNodeType::COND_BROADCAST:
    return "COND_BROADCAST";
  case SyncNodeType::BARRIER_WAIT:
    return "BARRIER_WAIT";
  case SyncNodeType::REGULAR_INST:
    return "REGULAR_INST";
  case SyncNodeType::FUNCTION_CALL:
    return "FUNCTION_CALL";
  case SyncNodeType::FUNCTION_RETURN:
    return "FUNCTION_RETURN";
  }
  return "UNKNOWN";
}

bool isSynchronizationNode(SyncNodeType type) {
  return type == SyncNodeType::LOCK_ACQUIRE ||
         type == SyncNodeType::LOCK_RELEASE ||
         type == SyncNodeType::COND_WAIT || type == SyncNodeType::COND_SIGNAL ||
         type == SyncNodeType::COND_BROADCAST ||
         type == SyncNodeType::BARRIER_WAIT;
}

bool isThreadBoundaryNode(SyncNodeType type) {
  return type == SyncNodeType::THREAD_START ||
         type == SyncNodeType::THREAD_FORK ||
         type == SyncNodeType::THREAD_JOIN || type == SyncNodeType::THREAD_EXIT;
}

} // namespace mhp

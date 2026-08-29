#ifndef DATAFLOW_APA_LLVM_FORWARDPROBLEM_H_
#define DATAFLOW_APA_LLVM_FORWARDPROBLEM_H_

#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/ControlFlow/IntraCFG.h"

#include <cstddef>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace elimination {

// Solver-facing types for LLVM IR: nodes and transfers are instructions, while
// the fact type is supplied by a concrete abstract domain.
template <typename FactT> struct LLVMAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = FactT;
  using transfer_t = llvm::Instruction *;
};

// LLVM-backed intraprocedural elimination problem for a single function.
// - Nodes are LLVM instructions.
// - Edge transfer defaults to the source instruction (forward IN facts).
// - Dominator-based reducible information is provided for ADT algorithms.
template <typename FactT>
class LLVMIntraEliminationProblem
    : public IntraReducibleEliminationProblem<LLVMAnalysisTypes<FactT>> {
public:
  using AnalysisTypes = LLVMAnalysisTypes<FactT>;
  using Base = IntraReducibleEliminationProblem<AnalysisTypes>;
  using n_t = typename Base::n_t;
  using fact_t = typename Base::fact_t;
  using transfer_t = typename Base::transfer_t;
  using Edge = typename Base::Edge;

  explicit LLVMIntraEliminationProblem(llvm::Function *F) : F(F) {}

  std::vector<n_t> nodes() const override {
    ensurePrepared();
    return Nodes;
  }

  n_t entry() const override {
    if (F == nullptr || F->isDeclaration()) {
      return nullptr;
    }
    return &*F->getEntryBlock().begin();
  }

  std::vector<n_t> succs(n_t Node) const override {
    return CFG.getSuccsOf(Node, dataflow::controlflow::FlowDirection::Forward);
  }

  // By default, we associate the transfer with the source instruction so that
  // the path expression for a node yields its IN facts (before the node runs).
  transfer_t edgeTransfer(n_t Src, n_t /*Dst*/) const override { return Src; }

  // Reducible (ADT) support.
  std::vector<Edge> edges() const override {
    ensurePrepared();
    return Edges;
  }

  std::vector<n_t> topologicalOrder() const override {
    ensurePrepared();
    return Topo;
  }

  n_t idom(n_t Node) const override {
    ensurePrepared();
    if (Node == nullptr || F == nullptr || F->isDeclaration()) {
      return Node;
    }
    const auto *BB = Node->getParent();
    if (Node == &*BB->begin()) {
      if (BB == &F->getEntryBlock()) {
        return Node;
      }
      const auto *DTNode = DT.getNode(const_cast<llvm::BasicBlock *>(BB));
      if (DTNode == nullptr || DTNode->getIDom() == nullptr) {
        return Node;
      }
      const auto *IDomBB = DTNode->getIDom()->getBlock();
      return IDomBB ? const_cast<n_t>(IDomBB->getTerminator()) : Node;
    }
    return Node->getPrevNode();
  }

  bool dominates(n_t A, n_t B) const override {
    ensurePrepared();
    if (A == nullptr || B == nullptr) {
      return false;
    }
    if (F == nullptr || F->isDeclaration()) {
      return false;
    }
    if (A == B) {
      return true;
    }
    const auto *BBA = A->getParent();
    const auto *BBB = B->getParent();
    if (BBA == BBB) {
      return A->comesBefore(B);
    }
    return DT.dominates(BBA, BBB);
  }

private:
  void ensurePrepared() const {
    if (Prepared) {
      return;
    }
    Prepared = true;

    Nodes.clear();
    Edges.clear();
    Topo.clear();
    Index.clear();

    if (F == nullptr || F->isDeclaration()) {
      return;
    }

    DT.recalculate(*F);

    // Restrict to nodes reachable from the entry to match the paper's
    // flowgraph assumption.
    std::unordered_set<n_t> Reach;
    Reach.reserve(64);
    auto *Entry = entry();
    if (Entry == nullptr) {
      return;
    }
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      auto *Cur = Stack.back();
      Stack.pop_back();
      for (auto *Succ :
           CFG.getSuccsOf(Cur, dataflow::controlflow::FlowDirection::Forward)) {
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }

    const auto AllNodes = CFG.getAllInstructionsOf(F);
    for (auto *N : AllNodes) {
      if (!Reach.count(N)) {
        continue;
      }
      Index.emplace(N, Nodes.size());
      Nodes.push_back(N);
    }

    const auto RawEdges = CFG.getAllControlFlowEdges(
        F, dataflow::controlflow::FlowDirection::Forward);
    for (const auto &E : RawEdges) {
      if (!Reach.count(E.first) || !Reach.count(E.second)) {
        continue;
      }
      Edges.push_back({E.first, E.second});
    }

    buildTopo();
  }

  void buildTopo() const {
    if (Nodes.empty()) {
      return;
    }

    std::unordered_map<n_t, std::vector<n_t>> Succ;
    std::unordered_map<n_t, std::size_t> InDeg;
    Succ.reserve(Nodes.size());
    InDeg.reserve(Nodes.size());
    for (auto *N : Nodes) {
      InDeg[N] = 0;
    }

    for (const auto &E : Edges) {
      if (this->isBackEdge(E.Src, E.Dst)) {
        continue;
      }
      Succ[E.Src].push_back(E.Dst);
      ++InDeg[E.Dst];
    }

    std::deque<n_t> Ready;
    auto *Entry = entry();
    if (Entry != nullptr) {
      Ready.push_back(Entry);
    }
    for (auto *N : Nodes) {
      if (N == Entry) {
        continue;
      }
      if (InDeg[N] == 0) {
        Ready.push_back(N);
      }
    }

    while (!Ready.empty()) {
      auto *Cur = Ready.front();
      Ready.pop_front();
      Topo.push_back(Cur);
      auto It = Succ.find(Cur);
      if (It == Succ.end()) {
        continue;
      }
      for (auto *S : It->second) {
        auto DegIt = InDeg.find(S);
        if (DegIt == InDeg.end()) {
          continue;
        }
        if (--DegIt->second == 0) {
          Ready.push_back(S);
        }
      }
    }

    if (Topo.size() != Nodes.size()) {
      Topo.clear();
    }
  }

  llvm::Function *F = nullptr;
  dataflow::controlflow::LLVMIntraCFG CFG;
  mutable llvm::DominatorTree DT;
  mutable bool Prepared = false;

  mutable std::vector<n_t> Nodes;
  mutable std::vector<Edge> Edges;
  mutable std::vector<n_t> Topo;
  mutable std::unordered_map<n_t, std::size_t> Index;
};

} // namespace elimination

#endif // DATAFLOW_APA_LLVM_FORWARDPROBLEM_H_

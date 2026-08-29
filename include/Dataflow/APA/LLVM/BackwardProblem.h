#ifndef DATAFLOW_APA_LLVM_BACKWARDPROBLEM_H_
#define DATAFLOW_APA_LLVM_BACKWARDPROBLEM_H_

// Backward-flow intraprocedural elimination problem (e.g., liveness, very
// busy). Uses PostDominatorTree: dominators in the backward CFG =
// post-dominators in the forward CFG. Provides reducible view for efficient ADT
// algorithms.

#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/LLVM/ForwardProblem.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/ControlFlow/IntraCFG.h"

#include <cstddef>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace elimination {

// Base for backward-flow elimination problems with reducible (ADT) support.
// Entry = exit instruction (e.g., ret). Succs = backward successors.
template <typename FactT>
class LLVMReverseIntraEliminationProblem
    : public IntraReducibleEliminationProblem<LLVMAnalysisTypes<FactT>> {
public:
  using AnalysisTypes = LLVMAnalysisTypes<FactT>;
  using Base = IntraReducibleEliminationProblem<AnalysisTypes>;
  using n_t = typename Base::n_t;
  using fact_t = typename Base::fact_t;
  using transfer_t = typename Base::transfer_t;
  using Edge = typename Base::Edge;

  explicit LLVMReverseIntraEliminationProblem(llvm::Function *F, n_t Entry)
      : F(F), Entry(Entry) {}

  std::vector<n_t> nodes() const override {
    ensurePrepared();
    return Nodes;
  }

  n_t entry() const override { return Entry; }

  std::vector<n_t> succs(n_t Node) const override {
    return CFG.getSuccsOf(Node, dataflow::controlflow::FlowDirection::Backward);
  }

  // Transfer associated with destination (backward: we enter Dst from Src).
  transfer_t edgeTransfer(n_t /*Src*/, n_t Dst) const override { return Dst; }

  std::vector<Edge> edges() const override {
    ensurePrepared();
    return Edges;
  }

  std::vector<n_t> topologicalOrder() const override {
    ensurePrepared();
    return Topo;
  }

  // In backward CFG, dominators = post-dominators in forward CFG.
  // For instruction-level backward CFG:
  //   - The "entry" of a backward traversal within a block is the terminator.
  //   - Within a block, the backward successor of instruction I is
  //   I->getPrevNode().
  //   - So the idom of a non-terminator instruction I (in the backward CFG) is
  //     I->getNextNode() (the instruction that immediately precedes I in the
  //     backward traversal, i.e., the next instruction in forward order).
  //   - For the terminator of a block, the idom is the terminator of the
  //     post-dominator block (the block that post-dominates this block).
  n_t idom(n_t Node) const override {
    ensurePrepared();
    if (Node == nullptr || F == nullptr || F->isDeclaration()) {
      return Node;
    }
    if (Node == Entry) {
      return Entry;
    }
    const auto *BB = Node->getParent();
    // Non-terminator: its backward-CFG idom is the next instruction in forward
    // order (which is its backward predecessor within the block).
    if (!Node->isTerminator()) {
      auto *Next = Node->getNextNode();
      return Next ? Next : Entry;
    }
    // Terminator: cross-block idom via post-dominator tree.
    const auto *PDTNode = PDT.getNode(const_cast<llvm::BasicBlock *>(BB));
    if (PDTNode == nullptr || PDTNode->getIDom() == nullptr) {
      return Entry;
    }
    const auto *IDomBB = PDTNode->getIDom()->getBlock();
    if (IDomBB == nullptr) {
      return Entry;
    }
    return const_cast<n_t>(IDomBB->getTerminator());
  }

  // A dominates B in backward CFG <=> B post-dominates A in forward CFG.
  bool dominates(n_t A, n_t B) const override {
    ensurePrepared();
    if (A == nullptr || B == nullptr) {
      return false;
    }
    if (A == B) {
      return true;
    }
    if (F == nullptr || F->isDeclaration()) {
      return false;
    }
    const auto *BBA = A->getParent();
    const auto *BBB = B->getParent();
    if (BBA == BBB) {
      return B->comesBefore(A); // In backward CFG, A dominates B means A is
    } // closer to exit; in same block, A is later.
    return PDT.dominates(BBA, BBB);
  }

protected:
  llvm::Function *F = nullptr;
  n_t Entry = nullptr;
  dataflow::controlflow::LLVMIntraCFG CFG;
  mutable llvm::PostDominatorTree PDT;
  mutable bool Prepared = false;

  mutable std::vector<n_t> Nodes;
  mutable std::vector<Edge> Edges;
  mutable std::vector<n_t> Topo;
  mutable std::unordered_map<n_t, std::size_t> Index;

  void ensurePrepared() const {
    if (Prepared) {
      return;
    }
    Prepared = true;

    Nodes.clear();
    Edges.clear();
    Topo.clear();
    Index.clear();

    if (F == nullptr || F->isDeclaration() || Entry == nullptr) {
      return;
    }

    PDT.recalculate(*F);

    std::unordered_set<n_t> Reach;
    Reach.reserve(64);
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      auto *Cur = Stack.back();
      Stack.pop_back();
      for (auto *Succ : CFG.getSuccsOf(
               Cur, dataflow::controlflow::FlowDirection::Backward)) {
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
        F, dataflow::controlflow::FlowDirection::Backward);
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
    Ready.push_back(Entry);
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
};

} // namespace elimination

#endif // DATAFLOW_APA_LLVM_BACKWARDPROBLEM_H_

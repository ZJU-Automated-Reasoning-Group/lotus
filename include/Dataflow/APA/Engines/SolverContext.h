#ifndef DATAFLOW_APA_ENGINES_SOLVERCONTEXT_H_
#define DATAFLOW_APA_ENGINES_SOLVERCONTEXT_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Core/Result.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace elimination {
namespace detail {

// Shared state and helper routines for all APA solver engines.
//
// The public solver facade owns one context instance and passes it to the
// selected engine. This keeps the per-engine headers focused on their
// algorithmic differences while centralizing common graph, ADT, and expression
// utilities in one place.
template <typename AnalysisDomainTy> class IntraEliminationSolverContext {
public:
  using ProblemTy = IntraEliminationProblem<AnalysisDomainTy>;
  using ReducibleProblemTy = IntraReducibleEliminationProblem<AnalysisDomainTy>;
  using n_t = typename ProblemTy::n_t;
  using fact_t = typename ProblemTy::fact_t;
  using transfer_t = typename ProblemTy::transfer_t;

  using expr_factory_t = PathExprFactory<transfer_t>;
  using expr_ref_t = typename expr_factory_t::Ref;
  using result_t = DataFlowResultT<n_t, fact_t, transfer_t>;

  // Normalized edge representation used by the internal reducible-view logic
  // and the ADT construction.
  struct Edge final {
    n_t Src{};
    n_t Dst{};
  };

  struct ADTNode final {
    bool Leaf = false;
    n_t FlowNode{};
    ADTNode *Left = nullptr;
    ADTNode *Right = nullptr;
    ADTNode *Parent = nullptr;
    std::size_t Depth = 0;
    // Entry node of the interval represented by this ADT node. For internal
    // nodes this is the left child's entry, matching the decomposition-tree
    // conventions used in the elimination papers.
    n_t Entry{};
    // Inclusive leaf-position range in reducible topological order.
    int MinPos = 0;
    int MaxPos = 0;
    // Cross edges classified at this composition node.
    std::vector<Edge> F;
    std::vector<Edge> B;
    // Delayed-engine union-find metadata.
    ADTNode *UFParent = nullptr;
    expr_ref_t UFExpr;
    // Simple-engine eagerly propagated expression for the leaf/interval.
    expr_ref_t SimpleExpr;
  };

  // LCA table over the ADT. The F/B-set computation uses this to identify the
  // lowest composition node whose left/right children are crossed by a CFG
  // edge.
  struct LCATable final {
    std::vector<ADTNode *> Euler;
    std::vector<int> Depth;
    std::unordered_map<ADTNode *, int> First;
    std::vector<std::vector<int>> Sparse;
    std::vector<int> Log2;

    void build(ADTNode *Root) {
      Euler.clear();
      Depth.clear();
      First.clear();
      Sparse.clear();
      Log2.clear();
      if (!Root) {
        return;
      }

      struct Frame {
        ADTNode *Node = nullptr;
        int Depth = 0;
        int Stage = 0;
      };

      std::vector<Frame> Stack;
      Stack.push_back({Root, 0, 0});
      while (!Stack.empty()) {
        auto Frame = Stack.back();
        Stack.pop_back();
        if (!Frame.Node) {
          continue;
        }
        if (Frame.Stage == 0) {
          if (!First.count(Frame.Node)) {
            First.emplace(Frame.Node, static_cast<int>(Euler.size()));
          }
          Euler.push_back(Frame.Node);
          Depth.push_back(Frame.Depth);

          if (Frame.Node->Right) {
            Stack.push_back({Frame.Node, Frame.Depth, 2});
            Stack.push_back({Frame.Node->Right, Frame.Depth + 1, 0});
          }
          if (Frame.Node->Left) {
            Stack.push_back({Frame.Node, Frame.Depth, 1});
            Stack.push_back({Frame.Node->Left, Frame.Depth + 1, 0});
          }
          continue;
        }

        Euler.push_back(Frame.Node);
        Depth.push_back(Frame.Depth);
      }

      const int M = static_cast<int>(Euler.size());
      if (M == 0) {
        return;
      }
      Log2.resize(M + 1);
      Log2[1] = 0;
      for (int i = 2; i <= M; ++i) {
        Log2[i] = Log2[i / 2] + 1;
      }

      const int K = Log2[M];
      Sparse.assign(K + 1, std::vector<int>(M));
      for (int i = 0; i < M; ++i) {
        Sparse[0][i] = i;
      }
      for (int k = 1; k <= K; ++k) {
        const int Len = 1 << k;
        const int Half = Len >> 1;
        for (int i = 0; i + Len <= M; ++i) {
          const int I1 = Sparse[k - 1][i];
          const int I2 = Sparse[k - 1][i + Half];
          Sparse[k][i] = (Depth[I1] <= Depth[I2]) ? I1 : I2;
        }
      }
    }

    ADTNode *query(ADTNode *A, ADTNode *B) const {
      if (!A || !B) {
        return nullptr;
      }
      auto ItA = First.find(A);
      auto ItB = First.find(B);
      if (ItA == First.end() || ItB == First.end()) {
        return nullptr;
      }
      int L = ItA->second;
      int R = ItB->second;
      if (L > R) {
        std::swap(L, R);
      }
      const int Len = R - L + 1;
      const int K = Log2[Len];
      const int I1 = Sparse[K][L];
      const int I2 = Sparse[K][R - (1 << K) + 1];
      return (Depth[I1] <= Depth[I2]) ? Euler[I1] : Euler[I2];
    }
  };

  struct ReducibleViewProvided final {
    const ReducibleProblemTy *R = nullptr;
    std::vector<Edge> Edges;
    std::vector<n_t> Topo;

    explicit ReducibleViewProvided(const ReducibleProblemTy &R) : R(&R) {}

    bool init() {
      Topo = R->topologicalOrder();
      if (Topo.empty()) {
        return false;
      }
      Edges.clear();
      const auto REdges = R->edges();
      Edges.reserve(REdges.size());
      for (const auto &E : REdges) {
        Edges.push_back({E.Src, E.Dst});
      }
      return true;
    }

    const std::vector<Edge> &edges() const { return Edges; }
    const std::vector<n_t> &topologicalOrder() const { return Topo; }
    n_t idom(n_t N) const { return R->idom(N); }
    bool dominates(n_t A, n_t B) const { return R->dominates(A, B); }
    bool isBackEdge(n_t Src, n_t Dst) const { return R->isBackEdge(Src, Dst); }
    transfer_t edgeTransfer(n_t Src, n_t Dst) const {
      return R->edgeTransfer(Src, Dst);
    }
  };

  // Reducible-graph metadata synthesized from a plain CFG when the client does
  // not implement IntraReducibleEliminationProblem directly.
  struct ComputedReducibleView final {
    const ProblemTy *Problem = nullptr;
    std::vector<n_t> Nodes;
    std::vector<Edge> Edges;
    std::vector<n_t> Topo;
    std::unordered_map<n_t, std::size_t> NodeIndex;
    std::vector<std::vector<std::size_t>> Preds;
    std::vector<n_t> Idom;
    std::vector<std::vector<std::size_t>> DomTreeChildren;
    std::vector<std::size_t> DomTin;
    std::vector<std::size_t> DomTout;

    const std::vector<Edge> &edges() const { return Edges; }
    const std::vector<n_t> &topologicalOrder() const { return Topo; }
    n_t idom(n_t N) const { return Idom.at(NodeIndex.at(N)); }
    bool dominates(n_t A, n_t B) const {
      const auto IA = NodeIndex.at(A);
      const auto IB = NodeIndex.at(B);
      return DomTin[IA] <= DomTin[IB] && DomTout[IB] <= DomTout[IA];
    }
    bool isBackEdge(n_t Src, n_t Dst) const { return dominates(Dst, Src); }
    transfer_t edgeTransfer(n_t Src, n_t Dst) const {
      return Problem->edgeTransfer(Src, Dst);
    }
  };

  explicit IntraEliminationSolverContext(const ProblemTy &Problem,
                                         const EliminationOptions &Opts)
      : Problem(Problem), Opts(Opts) {}

  // Populate interval entries and leaf ranges after the ADT shape is built.
  void computeEntriesAndRanges(ADTNode *N,
                               const std::unordered_map<n_t, int> &TopoPos) {
    if (!N) {
      return;
    }
    if (N->Leaf) {
      N->Entry = N->FlowNode;
      const auto It = TopoPos.find(N->FlowNode);
      assert(It != TopoPos.end());
      N->MinPos = It->second;
      N->MaxPos = It->second;
      return;
    }
    computeEntriesAndRanges(N->Left, TopoPos);
    computeEntriesAndRanges(N->Right, TopoPos);
    assert(N->Left && N->Right);
    N->Entry = N->Left->Entry;
    N->MinPos = std::min(N->Left->MinPos, N->Right->MinPos);
    N->MaxPos = std::max(N->Left->MaxPos, N->Right->MaxPos);
  }

  void initUF(ADTNode *N) {
    if (!N) {
      return;
    }
    N->UFParent = N;
    N->UFExpr = Exprs.one();
    if (N->Left) {
      initUF(N->Left);
    }
    if (N->Right) {
      initUF(N->Right);
    }
  }

  void linkUpdate(ADTNode *Parent, ADTNode *Child, const expr_ref_t &Prefix) {
    assert(Parent && Child);
    Child->UFParent = Parent;
    Child->UFExpr = Prefix;
  }

  // Compose delayed prefixes on demand, compressing the path to the ADT root.
  expr_ref_t evalUF(ADTNode *X) {
    assert(X);
    if (X->UFParent == X) {
      return X->UFExpr;
    }
    auto ParentExpr = evalUF(X->UFParent);
    X->UFExpr = Exprs.concat(ParentExpr, X->UFExpr);
    X->UFParent = X->UFParent->UFParent;
    return X->UFExpr;
  }

  // Build reducible metadata from a plain CFG.
  //
  // The ADT engines require stronger invariants than state elimination. This
  // routine rejects the ADT path unless all of the following hold:
  //   1) every listed node is reachable from entry,
  //   2) immediate dominators can be computed for all nodes,
  //   3) removing back edges yields an acyclic graph with entry first in topo.
  // When any check fails, the caller falls back to StateElimination.
  bool buildComputedReducibleView(ComputedReducibleView &Out) const {
    Out = ComputedReducibleView{};
    Out.Problem = &Problem;
    Out.Nodes = Problem.nodes();
    if (Out.Nodes.empty()) {
      return false;
    }

    Out.NodeIndex.clear();
    Out.NodeIndex.reserve(Out.Nodes.size());
    for (std::size_t i = 0; i < Out.Nodes.size(); ++i) {
      Out.NodeIndex.emplace(Out.Nodes[i], i);
    }

    const auto Entry = Problem.entry();
    const auto EntryIt = Out.NodeIndex.find(Entry);
    if (EntryIt == Out.NodeIndex.end()) {
      return false;
    }
    const std::size_t EntryIdx = EntryIt->second;

    // ADT-based methods only make sense on a connected intraprocedural region
    // rooted at entry, so reject problems with missing entry reachability.
    std::unordered_set<n_t> Reach;
    Reach.reserve(Out.Nodes.size());
    std::vector<n_t> Stack;
    Stack.push_back(Entry);
    Reach.insert(Entry);
    while (!Stack.empty()) {
      const auto Cur = Stack.back();
      Stack.pop_back();
      for (const auto &Succ : Problem.succs(Cur)) {
        if (Out.NodeIndex.find(Succ) == Out.NodeIndex.end()) {
          continue;
        }
        if (Reach.insert(Succ).second) {
          Stack.push_back(Succ);
        }
      }
    }
    if (Reach.size() != Out.Nodes.size()) {
      return false;
    }

    Out.Edges.clear();
    Out.Preds.assign(Out.Nodes.size(), {});
    for (const auto &Src : Out.Nodes) {
      const auto SrcIdx = Out.NodeIndex.at(Src);
      for (const auto &Dst : Problem.succs(Src)) {
        auto It = Out.NodeIndex.find(Dst);
        if (It == Out.NodeIndex.end()) {
          continue;
        }
        const auto DstIdx = It->second;
        Out.Edges.push_back({Src, Dst});
        Out.Preds[DstIdx].push_back(SrcIdx);
      }
    }

    const std::size_t N = Out.Nodes.size();
    std::vector<std::vector<std::size_t>> Succ(N);
    for (const auto &E : Out.Edges) {
      const auto SrcIdx = Out.NodeIndex.at(E.Src);
      const auto DstIdx = Out.NodeIndex.at(E.Dst);
      Succ[SrcIdx].push_back(DstIdx);
    }

    // Compute reverse postorder numbers from entry for iterative idom.
    std::vector<std::size_t> PostOrder;
    PostOrder.reserve(N);
    std::vector<std::size_t> NextSucc(N, 0);
    std::vector<std::size_t> DFSStack;
    std::vector<bool> Seen(N, false);
    DFSStack.push_back(EntryIdx);
    Seen[EntryIdx] = true;
    while (!DFSStack.empty()) {
      const auto Cur = DFSStack.back();
      auto &SI = NextSucc[Cur];
      if (SI < Succ[Cur].size()) {
        const auto Dst = Succ[Cur][SI++];
        if (!Seen[Dst]) {
          Seen[Dst] = true;
          DFSStack.push_back(Dst);
        }
        continue;
      }
      PostOrder.push_back(Cur);
      DFSStack.pop_back();
    }
    if (PostOrder.size() != N) {
      return false;
    }

    std::vector<std::size_t> RPO;
    RPO.reserve(N);
    for (auto It = PostOrder.rbegin(); It != PostOrder.rend(); ++It) {
      RPO.push_back(*It);
    }
    std::vector<std::size_t> Rank(N, 0);
    for (std::size_t I = 0; I < RPO.size(); ++I) {
      Rank[RPO[I]] = I;
    }

    std::vector<int> IdomIdx(N, -1);
    IdomIdx[EntryIdx] = static_cast<int>(EntryIdx);

    auto Intersect = [&](std::size_t A, std::size_t B) {
      while (A != B) {
        while (Rank[A] > Rank[B]) {
          A = static_cast<std::size_t>(IdomIdx[A]);
        }
        while (Rank[B] > Rank[A]) {
          B = static_cast<std::size_t>(IdomIdx[B]);
        }
      }
      return A;
    };

    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const auto V : RPO) {
        if (V == EntryIdx) {
          continue;
        }
        std::size_t NewIdom = static_cast<std::size_t>(-1);
        for (const auto P : Out.Preds[V]) {
          if (IdomIdx[P] == -1) {
            continue;
          }
          if (NewIdom == static_cast<std::size_t>(-1)) {
            NewIdom = P;
          } else {
            NewIdom = Intersect(P, NewIdom);
          }
        }
        if (NewIdom == static_cast<std::size_t>(-1)) {
          return false;
        }
        if (IdomIdx[V] != static_cast<int>(NewIdom)) {
          IdomIdx[V] = static_cast<int>(NewIdom);
          Changed = true;
        }
      }
    }

    Out.Idom.assign(N, Out.Nodes.front());
    for (std::size_t V = 0; V < N; ++V) {
      if (IdomIdx[V] == -1) {
        return false;
      }
      Out.Idom[V] = Out.Nodes[static_cast<std::size_t>(IdomIdx[V])];
    }

    Out.DomTreeChildren.assign(N, {});
    for (std::size_t V = 0; V < N; ++V) {
      if (V == EntryIdx) {
        continue;
      }
      Out.DomTreeChildren[static_cast<std::size_t>(IdomIdx[V])].push_back(V);
    }
    Out.DomTin.assign(N, 0);
    Out.DomTout.assign(N, 0);
    std::size_t Time = 0;
    struct DomFrame {
      std::size_t Node = 0;
      std::size_t ChildIdx = 0;
    };
    std::vector<DomFrame> DomStack;
    DomStack.push_back({EntryIdx, 0});
    while (!DomStack.empty()) {
      auto &Top = DomStack.back();
      if (Top.ChildIdx == 0) {
        Out.DomTin[Top.Node] = Time++;
      }
      if (Top.ChildIdx < Out.DomTreeChildren[Top.Node].size()) {
        const auto Child = Out.DomTreeChildren[Top.Node][Top.ChildIdx++];
        DomStack.push_back({Child, 0});
        continue;
      }
      Out.DomTout[Top.Node] = Time++;
      DomStack.pop_back();
    }

    // Topologically sort only the non-back edges. Failure here means the graph
    // violates reducibility assumptions required by the ADT engines.
    std::vector<std::vector<std::size_t>> SuccF(N);
    std::vector<std::size_t> InDeg(N, 0);
    for (const auto &E : Out.Edges) {
      const auto SrcIdx = Out.NodeIndex.at(E.Src);
      const auto DstIdx = Out.NodeIndex.at(E.Dst);
      if (Out.dominates(E.Dst, E.Src)) {
        continue;
      }
      SuccF[SrcIdx].push_back(DstIdx);
      ++InDeg[DstIdx];
    }

    if (InDeg[EntryIdx] != 0) {
      return false;
    }

    std::deque<std::size_t> Ready;
    Ready.push_back(EntryIdx);
    for (std::size_t i = 0; i < N; ++i) {
      if (i == EntryIdx) {
        continue;
      }
      if (InDeg[i] == 0) {
        Ready.push_back(i);
      }
    }

    Out.Topo.clear();
    Out.Topo.reserve(N);
    while (!Ready.empty()) {
      const auto Cur = Ready.front();
      Ready.pop_front();
      Out.Topo.push_back(Out.Nodes[Cur]);
      for (const auto S : SuccF[Cur]) {
        if (--InDeg[S] == 0) {
          Ready.push_back(S);
        }
      }
    }

    if (Out.Topo.size() != N) {
      return false;
    }
    if (Out.Topo.front() != Entry) {
      return false;
    }
    return true;
  }

  template <typename ReducibleViewT>
  bool prepareADT(const ReducibleViewT &R, ADTNode *&Root,
                  std::unordered_map<n_t, ADTNode *> &LeafOf,
                  std::unordered_map<n_t, int> &TopoPos,
                  std::vector<ADTNode *> &LeafByPos, LCATable &Lca) {
    const auto &Topo = R.topologicalOrder();
    if (Topo.empty()) {
      return false;
    }

    TopoPos.clear();
    TopoPos.reserve(Topo.size());
    for (int i = 0; i < static_cast<int>(Topo.size()); ++i) {
      TopoPos.emplace(Topo[i], i);
    }

    if (Topo.front() != Problem.entry()) {
      return false;
    }

    for (const auto &N : Problem.nodes()) {
      if (TopoPos.find(N) == TopoPos.end()) {
        return false;
      }
    }

    // Build the child stacks from the dominator tree as in the annotated
    // decomposition tree construction.
    std::unordered_map<n_t, std::vector<n_t>> Stacks;
    Stacks.reserve(Topo.size());
    for (const auto &N : Topo) {
      (void)Stacks[N];
    }

    for (int i = static_cast<int>(Topo.size()) - 1; i >= 1; --i) {
      const auto U = Topo[i];
      const auto V = R.idom(U);
      auto It = Stacks.find(V);
      if (It == Stacks.end()) {
        return false;
      }
      It->second.push_back(U);
    }

    // Allocate all ADT nodes from a single backing vector so pointers remain
    // stable once reserve() has been applied.
    ADTNodes.clear();
    ADTNodes.reserve(2 * Topo.size());
    LeafOf.clear();
    LeafOf.reserve(Topo.size());

    Root = traverseADT(Problem.entry(), Stacks, LeafOf);
    if (!Root) {
      return false;
    }
    Root->Parent = nullptr;
    computeEntriesAndRanges(Root, TopoPos);

    LeafByPos.assign(Topo.size(), nullptr);
    for (const auto &It : LeafOf) {
      const auto PosIt = TopoPos.find(It.first);
      if (PosIt == TopoPos.end()) {
        return false;
      }
      LeafByPos[PosIt->second] = It.second;
    }
    for (auto *Leaf : LeafByPos) {
      if (!Leaf) {
        return false;
      }
    }

    Lca.build(Root);
    if (!computeFBSets(R, Root, LeafOf, TopoPos, Lca)) {
      return false;
    }
    buildSelfLoopCache(R);
    return true;
  }

  ADTNode *newLeaf(n_t N, std::unordered_map<n_t, ADTNode *> &LeafOf) {
    ADTNodes.push_back({});
    auto *X = &ADTNodes.back();
    X->Leaf = true;
    X->FlowNode = N;
    LeafOf.emplace(N, X);
    return X;
  }

  ADTNode *newInner(ADTNode *L, ADTNode *R) {
    ADTNodes.push_back({});
    auto *X = &ADTNodes.back();
    X->Leaf = false;
    X->Left = L;
    X->Right = R;
    if (L) {
      L->Parent = X;
    }
    if (R) {
      R->Parent = X;
    }
    return X;
  }

  ADTNode *traverseADT(n_t U, std::unordered_map<n_t, std::vector<n_t>> &Stacks,
                       std::unordered_map<n_t, ADTNode *> &LeafOf) {
    auto It = Stacks.find(U);
    if (It == Stacks.end()) {
      return nullptr;
    }
    ADTNode *X = newLeaf(U, LeafOf);
    auto &S = It->second;
    while (!S.empty()) {
      const auto V = S.back();
      S.pop_back();
      ADTNode *Right = traverseADT(V, Stacks, LeafOf);
      if (!Right) {
        return nullptr;
      }
      X = newInner(X, Right);
    }
    return X;
  }

  template <typename ReducibleViewT>
  bool computeFBSets(const ReducibleViewT &R, ADTNode *Root,
                     const std::unordered_map<n_t, ADTNode *> &LeafOf,
                     const std::unordered_map<n_t, int> &TopoPos,
                     const LCATable &Lca) {
    (void)Root;
    // Classify each CFG edge at the lowest ADT composition node where it
    // crosses from one child interval to the other.
    for (const auto &E : R.edges()) {
      auto ItU = LeafOf.find(E.Src);
      auto ItV = LeafOf.find(E.Dst);
      if (ItU == LeafOf.end() || ItV == LeafOf.end()) {
        return false;
      }
      auto *A = ItU->second;
      auto *B = ItV->second;
      auto *X = Lca.query(A, B);
      if (!X || X->Leaf) {
        continue;
      }

      const auto PU = TopoPos.find(E.Src);
      const auto PV = TopoPos.find(E.Dst);
      if (PU == TopoPos.end() || PV == TopoPos.end()) {
        return false;
      }
      const int SrcPos = PU->second;
      const int DstPos = PV->second;
      auto *SrcChild = childContaining(X, SrcPos);
      auto *DstChild = childContaining(X, DstPos);
      if (!SrcChild || !DstChild || SrcChild == DstChild) {
        continue;
      }

      if (R.isBackEdge(E.Src, E.Dst)) {
        X->B.push_back(E);
      } else {
        X->F.push_back(E);
      }
    }
    return true;
  }

  template <typename ReducibleViewT>
  bool hasSelfLoop(const ReducibleViewT &R, n_t N) const {
    (void)R;
    auto It = SelfLoops.find(N);
    if (It == SelfLoops.end()) {
      return false;
    }
    return It->second;
  }

  fact_t eval(const expr_ref_t &E, const fact_t &In) const {
    assert(E && "expression must not be null");
    if (StarNonConvergent &&
        Opts.NonConvergentStarPolicy == OnNonConvergentStar::Fail) {
      return Problem.meetIdentity();
    }
    switch (E->K) {
    case expr_factory_t::Kind::Zero:
      return Problem.meetIdentity();
    case expr_factory_t::Kind::One:
      return In;
    case expr_factory_t::Kind::Atom:
      return Problem.applyTransfer(*E->Transfer, In);
    case expr_factory_t::Kind::Union: {
      auto L = eval(E->L, In);
      auto R = eval(E->R, In);
      return Problem.meet(L, R);
    }
    case expr_factory_t::Kind::Concat: {
      auto Mid = eval(E->L, In);
      return eval(E->R, Mid);
    }
    case expr_factory_t::Kind::Star: {
      // Star is interpreted by iterating until the user-defined lattice
      // stabilizes. This keeps the transfer-expression layer generic instead of
      // assuming a closed-form semiring implementation.
      auto Cur = In;
      const auto Limit = Opts.MaxStarIterations != 0
                             ? Opts.MaxStarIterations
                             : Problem.maxStarIterations();
      for (std::size_t i = 0; i < Limit; ++i) {
        ++Diagnostics.star_iterations_total;
        auto Next = Problem.meet(In, eval(E->L, Cur));
        if (Problem.equal_to(Next, Cur)) {
          return Cur;
        }
        Cur = std::move(Next);
      }
      Diagnostics.max_star_hit = true;
      switch (Opts.NonConvergentStarPolicy) {
      case OnNonConvergentStar::ReturnLast:
        return Cur;
      case OnNonConvergentStar::ReturnIdentity:
        return Problem.meetIdentity();
      case OnNonConvergentStar::Fail:
      default:
        StarNonConvergent = true;
        return Problem.meetIdentity();
      }
    }
    }
    if (Opts.NonConvergentStarPolicy == OnNonConvergentStar::Fail) {
      StarNonConvergent = true;
    }
    return Problem.meetIdentity();
  }

  std::size_t idx(const n_t &N) const {
    auto It = Index.find(N);
    assert(It != Index.end());
    return It->second;
  }

  const ProblemTy &Problem;
  EliminationOptions Opts;
  mutable bool StarNonConvergent = false;
  mutable SolveDiagnostics Diagnostics;
  expr_factory_t Exprs;
  std::vector<n_t> Nodes;
  std::unordered_map<n_t, std::size_t> Index;
  std::vector<std::vector<expr_ref_t>> Matrix;
  result_t Results;
  std::vector<ADTNode> ADTNodes;
  std::unordered_map<n_t, bool> SelfLoops;

private:
  static bool containsPos(const ADTNode *N, int Pos) {
    return N && N->MinPos <= Pos && Pos <= N->MaxPos;
  }

  static ADTNode *childContaining(ADTNode *W, int Pos) {
    if (!W || W->Leaf) {
      return nullptr;
    }
    if (containsPos(W->Left, Pos)) {
      return W->Left;
    }
    if (containsPos(W->Right, Pos)) {
      return W->Right;
    }
    return nullptr;
  }

  template <typename ReducibleViewT>
  void buildSelfLoopCache(const ReducibleViewT &R) {
    SelfLoops.clear();
    for (const auto &E : R.edges()) {
      if (E.Src == E.Dst) {
        SelfLoops[E.Src] = true;
      }
    }
  }
};

} // namespace detail
} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_SOLVERCONTEXT_H_

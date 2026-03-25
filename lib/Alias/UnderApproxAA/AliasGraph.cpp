/**
 * @file AliasGraph.cpp
 * @brief Alias graph data structure for must-alias analysis (CC'18)
 *
 * Implements the algorithms from "An Efficient Data Structure for Must-Alias
 * Analysis" (Kastrinis et al., CC'18): nodes = alias classes, edges = field
 * links; Move/Store/Load updates; intersect for merge points; gc; allAliases.
 */

#include "Alias/UnderApproxAA/AliasGraph.h"

#include <algorithm>
#include <queue>
#include <unordered_set>
#include <vector>

namespace UnderApprox {

//===----------------------------------------------------------------------===//
// Internal helpers
//===----------------------------------------------------------------------===//

NodeId AliasGraph::addNode() {
  NodeId Id = static_cast<NodeId>(nodes_.size());
  nodes_.emplace_back();
  inEdges_.emplace_back();
  return Id;
}

void AliasGraph::ensureInEdgesCapacity(NodeId N) {
  if (N >= inEdges_.size())
    inEdges_.resize(N + 1);
}

void AliasGraph::addEdge(NodeId From, FieldLabel F, NodeId To) {
  if (From >= nodes_.size() || To >= nodes_.size())
    return;
  auto It = nodes_[From].OutEdges.find(F);
  if (It != nodes_[From].OutEdges.end()) {
    NodeId OldTo = It->second;
    if (OldTo == To)
      return;
    removeInEdge(OldTo, From, F);
    It->second = To;
  } else {
    nodes_[From].OutEdges[F] = To;
  }
  addInEdge(To, From, F);
}

void AliasGraph::addInEdge(NodeId To, NodeId From, FieldLabel F) {
  ensureInEdgesCapacity(To);
  auto &Ins = inEdges_[To];
  for (const auto &P : Ins) {
    if (P.first == From && P.second == F)
      return;
  }
  Ins.push_back({From, F});
}

void AliasGraph::removeInEdge(NodeId To, NodeId From, FieldLabel F) {
  if (To >= inEdges_.size())
    return;
  auto &Ins = inEdges_[To];
  Ins.erase(std::remove_if(Ins.begin(), Ins.end(),
                           [From, F](const std::pair<NodeId, FieldLabel> &P) {
                             return P.first == From && P.second == F;
                           }),
            Ins.end());
}

void AliasGraph::removeVarFromNode(NodeId N, VarId V) {
  if (N >= nodes_.size())
    return;
  nodes_[N].Vars.erase(V);
  varToNode_.erase(V);
}

void AliasGraph::rebuildWithoutNodes(const llvm::SmallSet<NodeId, 16> &Remove) {
  const size_t n = nodes_.size();
  std::vector<NodeId> OldToNew(n, kNoNode);
  NodeId next = 0;
  for (NodeId N = 0; N < n; ++N) {
    if (Remove.count(N))
      continue;
    OldToNew[N] = next++;
  }

  AliasGraph NewG;
  NewG.nodes_.resize(next);
  NewG.inEdges_.resize(next);

  for (NodeId N = 0; N < n; ++N) {
    if (Remove.count(N))
      continue;
    NodeId M = OldToNew[N];
    NewG.nodes_[M].Vars = nodes_[N].Vars;
    for (VarId V : nodes_[N].Vars)
      NewG.varToNode_[V] = M;
    for (const auto &E : nodes_[N].OutEdges) {
      NodeId T = E.second;
      if (Remove.count(T))
        continue;
      NewG.nodes_[M].OutEdges[E.first] = OldToNew[T];
      NewG.inEdges_[OldToNew[T]].push_back({M, E.first});
    }
  }

  *this = std::move(NewG);
}

//===----------------------------------------------------------------------===//
// Copy
//===----------------------------------------------------------------------===//

AliasGraph::AliasGraph(const AliasGraph &Other)
    : nodes_(Other.nodes_), varToNode_(Other.varToNode_),
      inEdges_(Other.inEdges_) {}

AliasGraph &AliasGraph::operator=(const AliasGraph &Other) {
  if (this != &Other) {
    nodes_ = Other.nodes_;
    varToNode_ = Other.varToNode_;
    inEdges_ = Other.inEdges_;
  }
  return *this;
}

//===----------------------------------------------------------------------===//
// Node and variable management
//===----------------------------------------------------------------------===//

NodeId AliasGraph::addVariable(VarId V) {
  auto It = varToNode_.find(V);
  if (It != varToNode_.end())
    return It->second;
  NodeId N = addNode();
  nodes_[N].Vars.insert(V);
  varToNode_[V] = N;
  return N;
}

NodeId AliasGraph::getNode(VarId V) const {
  auto It = varToNode_.find(V);
  if (It == varToNode_.end())
    return kNoNode;
  return It->second;
}

NodeId AliasGraph::mergeNodes(NodeId I, NodeId J) {
  if (I == J)
    return I;
  if (I >= nodes_.size() || J >= nodes_.size())
    return I;

  Node &Ni = nodes_[I];
  Node &Nj = nodes_[J];

  // 1) Move all vars from J to I
  for (VarId V : Nj.Vars)
    varToNode_[V] = I;
  for (VarId V : Nj.Vars)
    Ni.Vars.insert(V);
  Nj.Vars.clear();

  // 2) Merge outgoing edges: for each field f in J, merge targets if I also has
  // f. Snapshot first to avoid iterator invalidation under recursive merges.
  llvm::SmallVector<std::pair<FieldLabel, NodeId>, 4> JOut;
  JOut.reserve(Nj.OutEdges.size());
  for (const auto &KV : Nj.OutEdges)
    JOut.push_back({KV.first, KV.second});

  for (const auto &KV : JOut) {
    FieldLabel F = KV.first;
    NodeId Tj = KV.second;
    removeInEdge(Tj, J, F);
    NodeId TjNorm = (Tj == J) ? I : Tj;
    auto It = Ni.OutEdges.find(F);
    if (It != Ni.OutEdges.end()) {
      NodeId Ti = It->second;
      NodeId TiNorm = (Ti == J) ? I : Ti;
      if (Ti != TiNorm)
        addEdge(I, F, TiNorm);
      if (TiNorm != TjNorm)
        mergeNodes(TiNorm, TjNorm);
      // After merge, Ti holds the merged node; I.OutEdges[F] already points to
      // Ti
    } else {
      Ni.OutEdges[F] = TjNorm;
      addInEdge(TjNorm, I, F);
    }
  }
  Nj.OutEdges.clear();

  // 3) Redirect all incoming edges of J to I
  ensureInEdgesCapacity(J);
  auto OldIn = inEdges_[J];
  for (const auto &P : OldIn) {
    NodeId Pred = P.first;
    FieldLabel F = P.second;
    if (Pred >= nodes_.size() || Pred == J)
      continue;
    auto It = nodes_[Pred].OutEdges.find(F);
    if (It == nodes_[Pred].OutEdges.end() || It->second != J)
      continue;
    addEdge(Pred, F, I);
  }
  inEdges_[J].clear();

  return I;
}

NodeId AliasGraph::moveMerge(VarId X, VarId Y) {
  NodeId Nx = addVariable(X);
  NodeId Ny = addVariable(Y);
  return mergeNodes(Nx, Ny);
}

void AliasGraph::storeEdge(VarId Base, FieldLabel F, VarId Target) {
  NodeId Nb = addVariable(Base);
  NodeId Nt = addVariable(Target);
  auto It = nodes_[Nb].OutEdges.find(F);
  if (It != nodes_[Nb].OutEdges.end() && It->second == Nt)
    return;
  addEdge(Nb, F, Nt);
}

void AliasGraph::loadEdge(VarId Base, FieldLabel F, VarId Z) {
  NodeId Nb = addVariable(Base);
  NodeId Nz = getNode(Z);
  if (Nz != kNoNode)
    removeVarFromNode(Nz, Z);
  Nz = addNode();
  nodes_[Nz].Vars.insert(Z);
  varToNode_[Z] = Nz;
  auto It = nodes_[Nb].OutEdges.find(F);
  if (It == nodes_[Nb].OutEdges.end()) {
    addEdge(Nb, F, Nz);
    return;
  }
  NodeId ExistingTarget = It->second;
  mergeNodes(ExistingTarget, Nz);
}

void AliasGraph::renameVariable(VarId OldId, VarId NewId) {
  if (OldId == NewId)
    return;
  NodeId No = getNode(OldId);
  if (No == kNoNode)
    return;
  removeVarFromNode(No, OldId);
  NodeId Nn = getNode(NewId);
  if (Nn == kNoNode) {
    nodes_[No].Vars.insert(NewId);
    varToNode_[NewId] = No;
  } else if (Nn != No) {
    mergeNodes(No, Nn);
  }
}

llvm::SmallVector<VarId, 4> AliasGraph::getNodeVars(NodeId N) const {
  llvm::SmallVector<VarId, 4> Out;
  if (N >= nodes_.size())
    return Out;
  for (VarId V : nodes_[N].Vars)
    Out.push_back(V);
  return Out;
}

bool AliasGraph::nodeEmpty(NodeId N) const {
  return N >= nodes_.size() || nodes_[N].Vars.empty();
}

NodeId AliasGraph::getTarget(NodeId N, FieldLabel F) const {
  if (N >= nodes_.size())
    return kNoNode;
  auto It = nodes_[N].OutEdges.find(F);
  if (It == nodes_[N].OutEdges.end())
    return kNoNode;
  return It->second;
}

void AliasGraph::getPredecessors(
    NodeId N, llvm::SmallVector<std::pair<NodeId, FieldLabel>, 4> &Out) const {
  Out.clear();
  if (N >= inEdges_.size())
    return;
  for (const auto &P : inEdges_[N])
    Out.push_back(P);
}

//===----------------------------------------------------------------------===//
// intersect (paper §4.1)
//===----------------------------------------------------------------------===//

AliasGraph AliasGraph::intersect(const AliasGraph &G1, const AliasGraph &G2) {
  AliasGraph Result;
  const size_t n1 = G1.nodes_.size();
  const size_t n2 = G2.nodes_.size();

  // Map (i,j) -> Result node id. Use linear index: (i, j) -> i * n2 + j
  llvm::DenseMap<uint64_t, NodeId> PairToNode;
  auto key = [n2](NodeId i, NodeId j) { return uint64_t(i) * n2 + j; };

  // Step 1: Materialize non-empty intersections using variable-to-node
  // indexing. For each node i in G1 and each var V in i, find j=node(V) in G2
  // and add V to (i,j).
  for (NodeId i = 0; i < n1; ++i) {
    if (G1.nodeEmpty(i))
      continue;
    for (VarId V : G1.nodes_[i].Vars) {
      auto ItG2 = G2.varToNode_.find(V);
      if (ItG2 == G2.varToNode_.end())
        continue;
      NodeId j = ItG2->second;
      uint64_t k = key(i, j);
      NodeId R;
      auto ItR = PairToNode.find(k);
      if (ItR == PairToNode.end()) {
        R = Result.addNode();
        PairToNode[k] = R;
      } else {
        R = ItR->second;
      }
      Result.nodes_[R].Vars.insert(V);
      Result.varToNode_[V] = R;
    }
  }

  // Step 2: Repeatedly: if (i,j) exists, for every label f with i--f-->k in G1
  // and j--f-->l in G2, add (k,l) if not present (vars = vars(k) ∩ vars(l)),
  // and edge (i,j)--f-->(k,l).
  std::vector<uint64_t> Worklist;
  for (const auto &KV : PairToNode)
    Worklist.push_back(KV.first);
  size_t idx = 0;
  while (idx < Worklist.size()) {
    uint64_t k = Worklist[idx++];
    NodeId i = static_cast<NodeId>(k / n2);
    NodeId j = static_cast<NodeId>(k % n2);
    NodeId Rij = PairToNode[k];

    for (const auto &E1 : G1.nodes_[i].OutEdges) {
      FieldLabel f = E1.first;
      NodeId k1 = E1.second;
      auto It2 = G2.nodes_[j].OutEdges.find(f);
      if (It2 == G2.nodes_[j].OutEdges.end())
        continue;
      NodeId l = It2->second;
      uint64_t kkl = key(k1, l);
      auto ItR = PairToNode.find(kkl);
      NodeId Rkl;
      if (ItR == PairToNode.end()) {
        Rkl = Result.addNode();
        PairToNode[kkl] = Rkl;
        Worklist.push_back(kkl);
        // Node (k1,l): vars = vars(k1) ∩ vars(l) (paper: "possibly empty")
        for (VarId V : G1.nodes_[k1].Vars) {
          if (G2.nodes_[l].Vars.count(V))
            Result.nodes_[Rkl].Vars.insert(V);
        }
        for (VarId V : Result.nodes_[Rkl].Vars)
          Result.varToNode_[V] = Rkl;
      } else {
        Rkl = ItR->second;
      }
      Result.addEdge(Rij, f, Rkl);
    }
  }

  // Paper §4.1: "Empty nodes with no in-edges can be eliminated eagerly."
  // (Full gc() would also remove single-var nodes with no edges, which can
  // remove meaningful result nodes.)
  Result.gcEmptyNodesWithNoInEdges();
  return Result;
}

//===----------------------------------------------------------------------===//
// gcEmptyNodesWithNoInEdges (paper §4.1 eager elimination in intersect)
//===----------------------------------------------------------------------===//

void AliasGraph::gcEmptyNodesWithNoInEdges() {
  while (true) {
    llvm::SmallSet<NodeId, 16> Remove;
    const size_t n = nodes_.size();
    for (NodeId N = 0; N < n; ++N) {
      if (!nodes_[N].Vars.empty())
        continue;
      size_t In = N < inEdges_.size() ? inEdges_[N].size() : 0;
      if (In == 0)
        Remove.insert(N);
    }
    if (Remove.empty())
      return;
    rebuildWithoutNodes(Remove);
  }
}

//===----------------------------------------------------------------------===//
// gc (paper §4.1)
//===----------------------------------------------------------------------===//

void AliasGraph::gc() {
  while (true) {
    llvm::SmallSet<NodeId, 16> Remove;
    const size_t n = nodes_.size();

    for (NodeId N = 0; N < n; ++N) {
      if (nodes_[N].Vars.empty()) {
        size_t In = N < inEdges_.size() ? inEdges_[N].size() : 0;
        size_t Out = nodes_[N].OutEdges.size();
        if (In == 0 || (In == 1 && Out == 0))
          Remove.insert(N);
      } else if (nodes_[N].Vars.size() == 1) {
        size_t In = N < inEdges_.size() ? inEdges_[N].size() : 0;
        size_t Out = nodes_[N].OutEdges.size();
        if (In == 0 && Out == 0)
          Remove.insert(N);
      }
    }
    if (Remove.empty())
      return;
    rebuildWithoutNodes(Remove);
  }
}

//===----------------------------------------------------------------------===//
// allAliases (paper §4.1)
//===----------------------------------------------------------------------===//

void AliasGraph::allAliases(
    VarId Base, const llvm::SmallVectorImpl<FieldLabel> &Path,
    unsigned maxLength,
    llvm::SmallVector<std::pair<VarId, llvm::SmallVector<FieldLabel, 4>>, 8>
        &Out) const {
  Out.clear();
  NodeId N = getNode(Base);
  if (N == kNoNode)
    return;
  for (FieldLabel F : Path) {
    N = getTarget(N, F);
    if (N == kNoNode)
      return;
  }
  // N is the node reached by Base + Path. Find all (var, path) that reach N
  // with path length <= maxLength. We do BFS backwards from N up to maxLength.
  struct State {
    NodeId Node;
    llvm::SmallVector<FieldLabel, 4> Path;
  };
  struct StateKey {
    NodeId Node;
    std::vector<FieldLabel> Path;

    bool operator==(const StateKey &Other) const {
      return Node == Other.Node && Path == Other.Path;
    }
  };
  struct StateKeyHash {
    size_t operator()(const StateKey &S) const {
      size_t H = std::hash<NodeId>()(S.Node);
      for (FieldLabel F : S.Path)
        H = (H * 1315423911u) ^ std::hash<FieldLabel>()(F + 0x9e3779b9u);
      return H;
    }
  };
  std::queue<State> Q;
  Q.push({N, {}});
  std::unordered_set<StateKey, StateKeyHash> Visited;

  while (!Q.empty()) {
    State S = Q.front();
    Q.pop();
    StateKey Key{S.Node, std::vector<FieldLabel>(S.Path.begin(), S.Path.end())};
    if (!Visited.insert(std::move(Key)).second)
      continue;

    for (VarId V : getNodeVars(S.Node))
      Out.push_back({V, S.Path});

    if (S.Path.size() >= maxLength)
      continue;

    if (S.Node >= inEdges_.size())
      continue;
    for (const auto &P : inEdges_[S.Node]) {
      NodeId Pred = P.first;
      FieldLabel F = P.second;
      llvm::SmallVector<FieldLabel, 4> NewPath(S.Path.begin(), S.Path.end());
      NewPath.insert(NewPath.begin(), F);
      Q.push({Pred, std::move(NewPath)});
    }
  }
}

bool AliasGraph::mustAliasAccessPath(
    VarId Base1, const llvm::SmallVectorImpl<FieldLabel> &Path1, VarId Base2,
    const llvm::SmallVectorImpl<FieldLabel> &Path2) const {
  NodeId N1 = getNode(Base1);
  NodeId N2 = getNode(Base2);
  if (N1 == kNoNode || N2 == kNoNode)
    return false;
  for (FieldLabel F : Path1) {
    N1 = getTarget(N1, F);
    if (N1 == kNoNode)
      return false;
  }
  for (FieldLabel F : Path2) {
    N2 = getTarget(N2, F);
    if (N2 == kNoNode)
      return false;
  }
  return N1 == N2;
}

} // end namespace UnderApprox

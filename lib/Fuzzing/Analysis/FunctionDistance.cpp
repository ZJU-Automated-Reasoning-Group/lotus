// CCS 18: Hawkeye: Towards a desired directed grey-box fuzzer
//
// This file implements function-level distance analysis for directed fuzzing.
// It computes call graph distances from each function to functions containing
// target locations, supporting both AFLGo (BFS) and Hawkeye (weighted) metrics.

#include "Fuzzing/Analysis/ExtendedCallGraph.h"
#include "Fuzzing/Analysis/FunctionDistance.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/ADT/BreadthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/Analysis/CallGraph.h>

#include <memory>
#include <utility>

using namespace llvm;

namespace {

// Represents a call graph node with reversed edges (callers instead of callees).
class InvertedCallGraphNode {
public:
  using CallRecord =
      std::pair<Optional<WeakTrackingVH>, InvertedCallGraphNode *>;

private:
  friend class InvertedCallGraph;

  const std::unique_ptr<CallGraphNode> &Inner;
  std::vector<CallRecord> Callers; // Non-owning pointers

public:
  using iterator = std::vector<CallRecord>::iterator;
  using const_iterator = std::vector<CallRecord>::const_iterator;

  inline iterator begin() { return Callers.begin(); }
  inline iterator end() { return Callers.end(); }
  inline const_iterator begin() const { return Callers.begin(); }
  inline const_iterator end() const { return Callers.end(); }

  InvertedCallGraphNode(const std::unique_ptr<CallGraphNode> &Inner)
      : Inner(Inner) {}

  const std::unique_ptr<CallGraphNode> &getInner() const { return Inner; }
};

// Inverted call graph for backward traversal from targets to callers.
class InvertedCallGraph {
  using FunctionMapTy =
      std::map<const Function *, std::unique_ptr<InvertedCallGraphNode>>;

  FunctionMapTy FunctionMap;

public:
  InvertedCallGraph(const CallGraph &CG) {
    // Construct all nodes
    for (auto &CGEntry : CG) {
      auto *F = CGEntry.first;
      auto &CGNode = CGEntry.second;
      FunctionMap[F] = std::make_unique<InvertedCallGraphNode>(CGNode);
    }

    // Fill caller references
    for (auto &CGEntry : CG) {
      auto *F = CGEntry.first;
      auto &CGNode = CGEntry.second;
      for (auto &CGNodePair : *CGNode) {
        auto &CallSite = CGNodePair.first;
        auto *Callee = CGNodePair.second;
        Function *CalleeF = Callee->getFunction();
        if (!CalleeF) {
          // Ignore external nodes
          continue;
        }

        FunctionMap[CalleeF]->Callers.push_back(
            std::make_pair(CallSite, FunctionMap[F].get()));
      }
    }
  }

  inline const InvertedCallGraphNode *operator[](const Function *F) const {
    const auto I = FunctionMap.find(F);
    return I->second.get();
  }
};

} // namespace

namespace llvm {
template <> struct GraphTraits<const InvertedCallGraphNode *> {
  using NodeRef = const InvertedCallGraphNode *;
  using CGNPairTy = InvertedCallGraphNode::CallRecord;
  using EdgeRef = const InvertedCallGraphNode::CallRecord &;

  static NodeRef getEntryNode(const InvertedCallGraphNode *StartICGNode) {
    return StartICGNode;
  }

  static NodeRef getValue(CGNPairTy P) { return P.second; }
  using ChildIteratorType =
      mapped_iterator<InvertedCallGraphNode::const_iterator,
                      decltype(&getValue)>;

  // NOLINTNEXTLINE(readability-identifier-naming)
  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N->begin(), &getValue);
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType(N->end(), &getValue);
  }
};
} // namespace llvm

// Computes Hawkeye distances using weighted Dijkstra. Edge weights depend on
// call site frequency: more call sites/BBs in caller reduce the edge distance.
std::map<Function *, double>
getHawkeyeDistancesFromFunction(Function &TargetFunction,
                                InvertedCallGraph &ICG) {
  using QueueItem = std::pair<double, const InvertedCallGraphNode *>;
  using QueueType = std::priority_queue<QueueItem, SmallVector<QueueItem>,
                                        std::greater<QueueItem>>;

  QueueType Queue;

  auto *TargetFunctionNode = ICG[&TargetFunction];
  Queue.emplace(0.0, TargetFunctionNode);

  std::map<Function *, double> DistancesFromTarget;
  while (!Queue.empty()) {
    auto CurrentDistance = Queue.top().first;
    auto *CurrentNode = Queue.top().second;
    Queue.pop();

    auto *CurrentFunction = CurrentNode->getInner()->getFunction();
    if (DistancesFromTarget.find(CurrentFunction) !=
        DistancesFromTarget.end()) {
      continue;
    }
    DistancesFromTarget[CurrentFunction] = CurrentDistance;

    std::map<InvertedCallGraphNode *, std::pair<double, double>> Calls;

    std::set<BasicBlock *> Seen;
    for (auto &Edge : *CurrentNode) {
      auto &Call = Edge.first;
      auto *CallerNode = Edge.second;
      if (!Call) {
        continue;
      }
      Calls[CallerNode].first++;

      auto *CB = cast<CallBase>(*Call);
      auto *BB = CB->getParent();
      if (Seen.find(BB) == Seen.end()) {
        Calls[CallerNode].second++;
        Seen.insert(BB);
      }
    }

    for (auto &Entry : Calls) {
      auto *CallerNode = Entry.first;
      auto &CallerNodeCounts = Entry.second;

      float CallSiteCoeff =
          (2 * CallerNodeCounts.first + 1) / (2 * CallerNodeCounts.first);
      float CallBBCoeff =
          (2 * CallerNodeCounts.second + 1) / (2 * CallerNodeCounts.second);
      float EdgeDistance = CallSiteCoeff * CallBBCoeff;

      Queue.emplace(CurrentDistance + EdgeDistance, CallerNode);
    }
  }

  return DistancesFromTarget;
}

// Computes AFLGo distances using simple BFS on inverted call graph.
// Distance is the call graph depth from target to each caller.
std::map<Function *, double>
getAFLGoDistancesFromFunction(Function &TargetFunction,
                              InvertedCallGraph &ICG) {
  auto *TargetFunctionNode = ICG[&TargetFunction];

  std::map<Function *, double> DistancesFromTarget;
  for (auto BFIter = bf_begin(TargetFunctionNode);
       BFIter != bf_end(TargetFunctionNode); ++BFIter) {
    DistancesFromTarget[BFIter->getInner()->getFunction()] = BFIter.getLevel();
  }

  return DistancesFromTarget;
}

AnalysisKey AFLGoFunctionDistanceAnalysis::Key;

// Computes distances from all functions to target-containing functions.
// Uses harmonic mean when a function can reach multiple targets.
AFLGoFunctionDistanceAnalysis::Result
AFLGoFunctionDistanceAnalysis::run(Module &M, ModuleAnalysisManager &MAM) {
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  CallGraph *CG = nullptr;
  if (!UseExtendedCG) {
    CG = &MAM.getResult<CallGraphAnalysis>(M);
  } else {
    CG = &MAM.getResult<ExtendedCallGraphAnalysis>(M);
  }

  InvertedCallGraph ICG{*CG};

  std::map<Function *, std::vector<double>> DistancesFromTargets;
  for (auto &F : M) {
    auto Targets = FAM.getResult<AFLGoTargetDetectionAnalysis>(F);
    if (Targets.BBs.empty()) {
      // F is not a target.
      continue;
    }

    std::map<Function *, double> Distances;
    if (!UseHawkeyeDistance) {
      Distances = getAFLGoDistancesFromFunction(F, ICG);
    } else {
      Distances = getHawkeyeDistancesFromFunction(F, ICG);
    }

    for (auto &DistanceEntry : Distances) {
      DistancesFromTargets[DistanceEntry.first].push_back(DistanceEntry.second);
    }
  }

  auto DistanceMap = DenseMap<Function *, double>();
  for (auto &DistancesFromTargetPair : DistancesFromTargets) {
    auto *Function = DistancesFromTargetPair.first;
    if (!Function)
      continue;

    auto &Distances = DistancesFromTargetPair.second;

    double HarmonicMean = 0;
    for (auto Distance : Distances) {
      HarmonicMean += 1.0 / Distance;
    }
    HarmonicMean = Distances.size() / HarmonicMean;

    DistanceMap[Function] = HarmonicMean;
  }

  return DistanceMap;
}
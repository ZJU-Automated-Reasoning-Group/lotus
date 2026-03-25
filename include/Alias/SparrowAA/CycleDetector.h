/**
 * @file CycleDetector.h
 * @brief Generic SCC-based cycle detector for Andersen's constraint graph.
 *
 * ## Algorithm
 *
 * `CycleDetector` implements **Nuutila's improved Tarjan SCC algorithm**
 * (Nuutila & Soisalon-Soininen, 1994).  The key difference from classic
 * Tarjan is the use of an `inComponent` set instead of a "lowlink" array,
 * which avoids redundant stack operations and is slightly faster in practice.
 *
 * The algorithm assigns a DFS timestamp to each node on first visit.  When
 * the DFS unwinds, if a node's timestamp equals its minimum reachable
 * timestamp (i.e., it is the root of an SCC), all nodes on the SCC stack
 * with a higher timestamp belong to the same SCC and are collapsed.
 *
 * ## Usage
 *
 * Subclass `CycleDetector<MyGraph>` and implement the three pure-virtual
 * hooks:
 *
 * ```cpp
 * class MyCycleDetector : public CycleDetector<MyGraph> {
 *   NodeType *getRep(NodeIndex n) override { ... }
 *   void processNodeOnCycle(const NodeType *n, const NodeType *rep) override {
 * ... } void processCycleRepNode(const NodeType *rep) override { ... } void
 * run() override { runOnGraph(&myGraph); }
 * };
 * ```
 *
 * - `getRep` must return the current representative of a (possibly merged)
 *   node.  During solving, nodes in the same SCC are merged; `getRep`
 *   follows the union-find chain.
 * - `processNodeOnCycle` is called for each non-representative node in an
 *   SCC.  Typically merges the node into the representative.
 * - `processCycleRepNode` is called once for the representative of each SCC
 *   (including trivial SCCs of size 1).
 *
 * @tparam GraphType  The concrete graph type; must have an
 *                    `AndersGraphTraits<GraphType>` specialisation.
 *
 * ## B1 Fix: Iterative DFS to Avoid Stack Overflow
 *
 * The original recursive `visit()` could overflow the native call stack on
 * large constraint graphs (deep chains of copy edges).  The implementation
 * now uses an explicit worklist to perform the same Nuutila SCC algorithm
 * iteratively.  Each worklist frame stores the node and its current child
 * iterator so that the DFS can be resumed after processing each child.
 * A separate `entryTime` map records the timestamp assigned when a node was
 * first pushed, allowing the SCC-root check (dfsNum[node] == entryTime) to
 * work correctly even after dfsNum[node] has been lowered by back-edges.
 */

#ifndef ANDERSEN_CYCLEDETECTOR_H
#define ANDERSEN_CYCLEDETECTOR_H

#include "Alias/SparrowAA/GraphTraits.h"

#include <stack>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

/**
 * @class CycleDetector
 * @brief Abstract base class for SCC detection in Andersen's constraint graph.
 *
 * Subclasses must implement `getRep`, `processNodeOnCycle`,
 * `processCycleRepNode`, and `run`.  The DFS traversal and SCC bookkeeping
 * are handled entirely by this base class.
 *
 * @tparam GraphType  The concrete graph type.
 */
template <class GraphType> class CycleDetector {
public:
  using GraphTraits = AndersGraphTraits<GraphType>;
  using NodeType = typename GraphTraits::NodeType;
  using node_iterator = typename GraphTraits::NodeIterator;
  using child_iterator = typename GraphTraits::ChildIterator;

private:
  /// Stack of nodes whose SCC membership has not yet been finalised.
  std::stack<const NodeType *> sccStack;

  /// Maps each visited node to its current minimum-reachable DFS timestamp
  /// (the "lowlink" equivalent in Nuutila's algorithm).
  llvm::DenseMap<const NodeType *, unsigned> dfsNum;

  /// Records the DFS discovery timestamp assigned when a node was first
  /// pushed onto the worklist.  Unlike dfsNum, this value is never lowered
  /// by back-edges and is used to detect SCC roots.
  llvm::DenseMap<const NodeType *, unsigned> entryTime;

  /// Tracks nodes that have been assigned to a completed SCC.
  llvm::DenseSet<const NodeType *> inComponent;

  /// Monotonically increasing DFS timestamp counter.
  unsigned timestamp;

  // -----------------------------------------------------------------------
  // B1 Fix: iterative DFS using an explicit worklist.
  //
  // Each WorkItem stores the node being visited and the iterator pointing to
  // the next child to process.  When childIt == childEnd all children have
  // been processed and we perform the post-order SCC-root check.
  // -----------------------------------------------------------------------
  struct WorkItem {
    NodeType *node;
    child_iterator childIt;
    child_iterator childEnd;
  };

  void visit(NodeType *startNode) {
    std::vector<WorkItem> worklist;

    // Push a node onto the worklist for the first time.
    auto pushNode = [&](NodeType *node) {
      unsigned ts = timestamp++;
      dfsNum[node] = ts;
      entryTime[node] = ts;
      worklist.push_back(
          {node, GraphTraits::child_begin(node), GraphTraits::child_end(node)});
    };

    pushNode(startNode);

    while (!worklist.empty()) {
      WorkItem &item = worklist.back();
      NodeType *node = item.node;

      if (item.childIt != item.childEnd) {
        // There is still a child to process.
        NodeType *succRep = getRep(*item.childIt);
        ++item.childIt;

        if (!dfsNum.count(succRep)) {
          // Tree edge: visit the successor.
          pushNode(succRep);
          // After pushNode returns we will re-enter the loop and process
          // the successor's children before coming back to this node.
        } else {
          // Back/cross edge: propagate the minimum timestamp upward.
          if (!inComponent.count(succRep) && dfsNum[node] > dfsNum[succRep])
            dfsNum[node] = dfsNum[succRep];
        }
      } else {
        // All children of `node` have been processed — post-order work.
        worklist.pop_back();

        // Propagate minimum timestamp to the parent frame (if any).
        if (!worklist.empty()) {
          NodeType *parent = worklist.back().node;
          if (!inComponent.count(node) && dfsNum[parent] > dfsNum[node])
            dfsNum[parent] = dfsNum[node];
        }

        // SCC-root check: node is the root of an SCC iff its dfsNum was
        // never lowered below its entry timestamp.
        unsigned myEntry = entryTime[node];
        if (dfsNum[node] != myEntry) {
          // Not an SCC root — push onto the SCC stack and continue.
          sccStack.push(node);
          continue;
        }

        // node is an SCC root: pop all nodes on the SCC stack that were
        // discovered after this node (they belong to the same SCC).
        inComponent.insert(node);
        while (!sccStack.empty()) {
          const NodeType *cycleNode = sccStack.top();
          if (dfsNum[cycleNode] < myEntry)
            break;
          processNodeOnCycle(cycleNode, node);
          inComponent.insert(cycleNode);
          sccStack.pop();
        }

        processCycleRepNode(node);
      }
    }
  }

protected:
  // Nodes may get merged during the analysis. This function returns the merge
  // target (if the node is merged into another node) or the node itself (if the
  // node has not been merged into another node).
  virtual NodeType *getRep(NodeIndex node) = 0;
  // Specify how to process the non-rep nodes if a cycle is found.
  virtual void processNodeOnCycle(const NodeType *node,
                                  const NodeType *repNode) = 0;
  // Specify how to process the rep nodes if a cycle is found.
  virtual void processCycleRepNode(const NodeType *node) = 0;

  // Running the cycle detection algorithm on a given graph G.
  void runOnGraph(GraphType *graph) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");
    assert(dfsNum.empty() && "dfsNum is not empty before cycle detection!");
    assert(inComponent.empty() &&
           "inComponent is not empty before cycle detection!");

    for (auto itr = GraphTraits::node_begin(graph),
              ite = GraphTraits::node_end(graph);
         itr != ite; ++itr) {
      NodeType *repNode = getRep(itr->getNodeIndex());
      if (!dfsNum.count(repNode))
        visit(repNode);
    }

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  // Running the cycle detection algorithm starting from a single node.
  // Used when walking the entire graph is not desirable.
  //
  // B7 Fix: the original implementation did not reset dfsNum, entryTime, or
  // inComponent between successive runOnNode calls.  On the second and later
  // calls, nodes visited in a prior call were already present in dfsNum and
  // were therefore skipped, causing cycles that span multiple candidate pairs
  // to be missed.  Fix: clear all per-run bookkeeping before each call so
  // that every runOnNode invocation starts with a clean slate.
  void runOnNode(NodeIndex node) {
    assert(sccStack.empty() && "sccStack is not empty before cycle detection!");

    // Reset per-run state so that nodes visited in a previous runOnNode call
    // are not incorrectly treated as already-visited.
    dfsNum.clear();
    entryTime.clear();
    inComponent.clear();
    timestamp = 0;

    NodeType *repNode = getRep(node);
    visit(repNode);

    assert(sccStack.empty() && "sccStack not empty after cycle detection!");
  }

  void releaseSCCMemory() {
    dfsNum.clear();
    entryTime.clear();
    inComponent.clear();
  }

public:
  CycleDetector() : timestamp(0) {}

  // The public interface of running the detector.
  virtual void run() = 0;
};

#endif

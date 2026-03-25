//===- SrcSnkSolver.h -- CFL reachability solver -------------------===//
//
// Migrated from SVF's SABER engine to Lotus.
// Generic CFL solver for demand-driven analysis.
//
//===----------------------------------------------------------------------===//

#ifndef SRC_SNK_SOLVER_H
#define SRC_SNK_SOLVER_H

#include "Alias/DDA/DPItem.h"
#include "IR/SVFG/SVFG.h"

#include <deque>
#include <set>

namespace lotus {
namespace analysis {

template <class DPIm> class SrcSnkSolver {
public:
  using WorkList = std::deque<DPIm>;

protected:
  SrcSnkSolver() : graph_(nullptr) {}
  virtual ~SrcSnkSolver() = default;

  const SVFG *graph() const { return graph_; }
  SVFG *graph() { return graph_; }

  void setGraph(SVFG *g) { graph_ = g; }

  SVFGNode *getNode(uint32_t id) const { return graph_->getNode(id); }

  virtual uint32_t getNodeIDFromItem(const DPIm &item) const {
    return item.getCurNodeID();
  }

  virtual void forwardTraverse(DPIm &it) {
    pushIntoWorklist(it);
    while (!isWorklistEmpty()) {
      DPIm item = popFromWorklist();
      FWProcessCurNode(item);
      SVFGNode *v = getNode(getNodeIDFromItem(item));
      if (!v)
        continue;
      for (auto &edge : v->getOutEdges()) {
        FWProcessOutgoingEdge(item, edge);
      }
    }
  }

  virtual void backwardTraverse(DPIm &it) {
    pushIntoWorklist(it);
    while (!isWorklistEmpty()) {
      DPIm item = popFromWorklist();
      BWProcessCurNode(item);
      SVFGNode *v = getNode(getNodeIDFromItem(item));
      if (!v)
        continue;
      for (auto &edge : v->getInEdges()) {
        BWProcessIncomingEdge(item, edge);
      }
    }
  }

  virtual void FWProcessCurNode(const DPIm &) {}
  virtual void BWProcessCurNode(const DPIm &) {}

  virtual void FWProcessOutgoingEdge(const DPIm &item, SVFGEdge *edge) {
    DPIm newItem(item);
    newItem.setCurNodeID(edge->getDstNode()->getId());
    pushIntoWorklist(newItem);
  }

  virtual void BWProcessIncomingEdge(const DPIm &item, SVFGEdge *edge) {
    DPIm newItem(item);
    newItem.setCurNodeID(edge->getSrcNode()->getId());
    pushIntoWorklist(newItem);
  }

  inline DPIm popFromWorklist() {
    DPIm item = worklist_.front();
    worklist_.pop_front();
    worklistSet_.erase(item);
    return item;
  }

  inline bool pushIntoWorklist(DPIm &item) {
    if (worklistSet_.find(item) != worklistSet_.end())
      return false;
    worklist_.push_back(item);
    worklistSet_.insert(item);
    return true;
  }

  inline bool isWorklistEmpty() const { return worklist_.empty(); }

  inline bool isInWorklist(DPIm &item) {
    return worklistSet_.find(item) != worklistSet_.end();
  }

  void clearWorklist() {
    worklist_.clear();
    worklistSet_.clear();
  }

private:
  SVFG *graph_ = nullptr;
  WorkList worklist_;
  std::set<DPIm> worklistSet_;
};

} // namespace analysis
} // namespace lotus

#endif

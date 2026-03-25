#pragma once

#include <cassert>
#include <unordered_map>

namespace util {

// A worklist that contains two levels: each element in the global level has its
// own local-level worklist, and the elements of one local-level worklist will
// not get processed until its corresponding global-level element is dequeued
// This data structure is typically used in an inter-procedural dataflow
// analysis to ensure that all local states of a function are all properly
// propagated until the next function can be processed
template <typename GlobalWorkList, typename LocalWorkList>
class TwoLevelWorkList {
public:
  using GlobalElemType = typename GlobalWorkList::ElemType;
  using LocalElemType = typename LocalWorkList::ElemType;
  using ElemType = std::pair<GlobalElemType, LocalElemType>;

private:
  GlobalWorkList globalWorkList;

  using WorkListMap = std::unordered_map<GlobalElemType, LocalWorkList>;
  WorkListMap workListMap;

public:
  TwoLevelWorkList() = default;

  void enqueue(ElemType elem) {
    globalWorkList.enqueue(elem.first);
    workListMap[elem.first].enqueue(elem.second);
  }

  ElemType dequeue() {
    assert(!empty());
    auto currGlobalElem = globalWorkList.front();
    auto &currLocalWorkList = workListMap[currGlobalElem];

    assert(!currLocalWorkList.empty());
    auto currLocalElem = currLocalWorkList.dequeue();

    if (currLocalWorkList.empty())
      globalWorkList.dequeue();

    return std::make_pair(currGlobalElem, currLocalElem);
  }

  ElemType front() {
    assert(!empty());
    auto currGlobalElem = globalWorkList.front();
    auto currLocalElem = workListMap[currGlobalElem].front();
    return std::make_pair(currGlobalElem, currLocalElem);
  }

  bool empty() const { return globalWorkList.empty(); }
};

} // namespace util

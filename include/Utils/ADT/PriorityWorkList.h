#pragma once

#include <cassert>
#include <queue>
#include <unordered_set>

namespace util {

// This worklist implementation is a priority queue with an additional set to
// help remove duplicate entries
template <typename T, typename Compare = std::less<T>,
          typename Hasher = std::hash<T>>
class PriorityWorkList {
public:
  using ElemType = T;

private:
  // The FIFO queue
  std::priority_queue<ElemType, std::vector<ElemType>, Compare> list;
  // Avoid duplicate entries in FIFO queue
  std::unordered_set<ElemType, Hasher> set;

public:
  PriorityWorkList() {}
  PriorityWorkList(const Compare &cmp) : list(cmp) {}

  bool enqueue(ElemType elem) {
    if (!set.count(elem)) {
      list.push(elem);
      set.insert(elem);
      return true;
    } else
      return false;
  }

  ElemType dequeue() {
    assert(!list.empty() && "Trying to dequeue an empty queue!");
    ElemType ret = list.top();
    list.pop();
    set.erase(ret);
    return ret;
  }

  ElemType front() {
    assert(!list.empty() && "Trying to dequeue an empty queue!");
    return list.top();
  }

  bool empty() const { return list.empty(); }
};

} // namespace util

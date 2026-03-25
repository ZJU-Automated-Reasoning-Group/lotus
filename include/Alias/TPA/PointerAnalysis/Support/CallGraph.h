#pragma once

#include "Alias/TPA/Util/DataStructure/VectorSet.h"
#include "Alias/TPA/Util/Iterator/IteratorRange.h"

#include <unordered_map>

namespace tpa {

template <typename CallerType, typename CalleeType> class CallGraph {
private:
  using CalleeSet = util::VectorSet<CalleeType>;
  using CallerSet = util::VectorSet<CallerType>;
  using CalleeConstIterator = typename CalleeSet::const_iterator;
  using CallerConstIterator = typename CallerSet::const_iterator;

  using CalleeMap = std::unordered_map<CallerType, CalleeSet>;
  using CallerMap = std::unordered_map<CalleeType, CallerSet>;

  CalleeMap calleeMap;
  CallerMap callerMap;

  template <typename MapType, typename KeyType, typename ValueType>
  static bool insertMap(MapType &m, const KeyType &k, const ValueType &v) {
    using SetType = util::VectorSet<ValueType>;

    auto mapInsertPair = m.insert(std::make_pair(k, SetType()));
    auto setInsertPair = mapInsertPair.first->second.insert(v);

    return mapInsertPair.second || setInsertPair.second;
  }

public:
  CallGraph() = default;

  bool insertEdge(const CallerType &caller, const CalleeType &callee) {
    auto ret0 = insertMap(calleeMap, caller, callee);
    auto ret1 = insertMap(callerMap, callee, caller);
    return ret0 || ret1;
  }

  util::IteratorRange<CalleeConstIterator>
  getCallees(const CallerType &caller) const {
    auto itr = calleeMap.find(caller);
    if (itr == calleeMap.end())
      return util::iteratorRange(CalleeConstIterator(), CalleeConstIterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
  }

  util::IteratorRange<CallerConstIterator>
  getCallers(const CalleeType &callee) const {
    auto itr = callerMap.find(callee);
    if (itr == callerMap.end())
      return util::iteratorRange(CallerConstIterator(), CallerConstIterator());
    else
      return util::iteratorRange(itr->second.begin(), itr->second.end());
  }
};

} // namespace tpa

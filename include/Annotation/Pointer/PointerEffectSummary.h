#pragma once

#include "Annotation/Pointer/PointerEffect.h"

#include <vector>

namespace annotation {

class PointerEffectSummary {
private:
  using EffectList = std::vector<PointerEffect>;
  EffectList list;

public:
  using const_iterator = EffectList::const_iterator;

  PointerEffectSummary() = default;

  void addEffect(PointerEffect &&e) { list.emplace_back(std::move(e)); }

  const_iterator begin() const { return list.begin(); }
  const_iterator end() const { return list.end(); }
  bool empty() const { return list.empty(); }
};

} // namespace annotation

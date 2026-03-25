#pragma once

#include "Annotation/ModRef/ModRefEffect.h"

#include <vector>

namespace annotation {

class ModRefEffectSummary {
private:
  using EffectList = std::vector<ModRefEffect>;
  EffectList list;

public:
  using const_iterator = EffectList::const_iterator;

  ModRefEffectSummary() = default;

  void addEffect(ModRefEffect &&e) { list.emplace_back(std::move(e)); }

  const_iterator begin() const { return list.begin(); }
  const_iterator end() const { return list.end(); }
  bool empty() const { return list.empty(); }
};

} // namespace annotation

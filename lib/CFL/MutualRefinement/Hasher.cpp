#include "CFL/MutualRefinement/Hasher.h"

#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>

// This parameter can be adjusted.
// The idea is that when the elements in the pair or triple are small integers,
// this implementation results in perfect hashing.
static constexpr long long SHIFT = 20LL;

std::size_t IntPairHasher::operator()(const std::pair<int, int> &p) const {
  return std::hash<long long>()((static_cast<long long>(p.first) << SHIFT) |
                                (static_cast<long long>(p.second)));
}

std::size_t
IntTripleHasher::operator()(const std::tuple<int, int, int> &t) const {
  return std::hash<long long>()(
      (static_cast<long long>(std::get<0>(t)) << (SHIFT << 1LL)) |
      (static_cast<long long>(std::get<1>(t)) << SHIFT) |
      (static_cast<long long>(std::get<2>(t))));
}

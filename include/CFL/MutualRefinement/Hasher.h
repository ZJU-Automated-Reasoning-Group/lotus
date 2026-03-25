#ifndef HASHER_H
#define HASHER_H

#include <cstddef>
#include <tuple>
#include <utility>

struct IntPairHasher {
  std::size_t operator()(const std::pair<int, int> &p) const;
};

struct IntTripleHasher {
  std::size_t operator()(const std::tuple<int, int, int> &t) const;
};

// An edge i --A--> j is represented as (i, A, j)
using Edge = std::tuple<int, int, int>;
using EdgeHasher = IntTripleHasher;

#endif
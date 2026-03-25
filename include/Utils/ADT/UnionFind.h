#pragma once

#include <vector>

class UnionFind final {
  std::vector<unsigned> id;

public:
  UnionFind(unsigned n = 0);
  unsigned mk();
  unsigned find(unsigned i);
  unsigned merge(unsigned p, unsigned q);
};

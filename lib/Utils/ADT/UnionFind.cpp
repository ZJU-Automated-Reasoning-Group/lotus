#include "Utils/ADT/UnionFind.h"

using namespace std;

// Constructs a Union-Find data structure with n elements.
UnionFind::UnionFind(unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    id.push_back(i);
  }
}

// Creates a new element and returns its ID.
unsigned UnionFind::mk() {
  unsigned n = id.size();
  id.push_back(n);
  return n;
}

// Finds the root of the set containing element i with path compression.
unsigned UnionFind::find(unsigned i) {
  unsigned root = i;
  while (root != id[root])
    root = id[root];
  id[i] = root;
  return root;
}

// Merges the sets containing elements p and q. Returns the root of merged set.
unsigned UnionFind::merge(unsigned p, unsigned q) {
  unsigned i = find(p);
  unsigned j = find(q);
  id[i] = j;
  return j;
}

#ifndef DISJOINT_SET_H
#define DISJOINT_SET_H

#include <iostream>
#include <vector>
using namespace std;

class DisjointSet {
public:
  vector<unsigned> arr;
  DisjointSet(unsigned size) {
    arr.reserve(size);
    for (unsigned i = 0; i < size; i++) {
      arr.push_back(i);
    }
  }

  unsigned find(unsigned idx) {
    if (arr[idx] != idx) {
      arr[idx] = find(arr[idx]);
    }
    return arr[idx];
  }

  void join(unsigned x, unsigned y) {
    unsigned xRoot = find(x);
    unsigned yRoot = find(y);
    arr[yRoot] = xRoot;
  }

  void print() {
    for (unsigned i = 0; i < arr.size(); i++) {
      std::cout << i << ", ";
    }
    std::cout << '\n';
    for (unsigned i = 0; i < arr.size(); i++) {
      std::cout << find(i) << ", ";
    }
    std::cout << '\n';
  }
};

#endif
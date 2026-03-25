/*
 * @author: rainoftime
 * @brief: OrderedSet is a set data structure that is ordered.
 * 一个有序集合实现，是其他有序容器的基础。它提供了高效的查找、插入和删除操作，同时保持了集合的顺序性。
 */

#pragma once

#include <vector>

using namespace std;

template <class T> class OrderedSet {
private:
  vector<T> vectors;

public:
  typedef typename vector<T>::iterator iterator;
  typedef typename vector<T>::const_iterator const_iterator;

  OrderedSet() {}

  OrderedSet(const OrderedSet &other) {
    this->vectors = std::move(other.vectors);
  }

  void erase(T v) {
    for (auto It = vectors.begin(); It != vectors.end(); It++) {
      if ((*It) == v) {
        It = vectors.erase(It);
        return;
      }
    }
  }

  const_iterator erase(const_iterator iter) { return vectors.erase(iter); }

  iterator erase(iterator iter) { return vectors.erase(iter); }

  bool insert(T v) {
    for (auto vec : vectors) {
      if (vec == v) {
        return false;
      }
    }
    vectors.push_back(v);
    return true;
  }

  const_iterator find(const T v) const {
    for (auto It = vectors.begin(); It != vectors.end(); It++) {
      if ((*It) == v) {
        return It;
      }
    }
    return vectors.end();
  }

  int count(const T v) const {
    int counter = 0;
    for (auto It = vectors.begin(); It != vectors.end(); It++) {
      if ((*It) == v) {
        counter++;
      }
    }
    return counter;
  }

  void pop_back() { vectors.pop_back(); }

  int size() { return vectors.size(); }

  inline iterator begin() { return vectors.begin(); }

  inline iterator end() { return vectors.end(); }

  inline const_iterator begin() const { return vectors.begin(); }

  inline const_iterator end() const { return vectors.end(); }
};

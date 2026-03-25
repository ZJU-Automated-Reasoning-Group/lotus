#pragma once

#include <unordered_map>

template <typename T> struct Node {
  T _val;
  int _rank;
  Node *_parent;

  Node(T val, int rank, Node *parent = nullptr)
      : _val(val), _rank(rank), _parent(parent) {}
};

template <typename T> class DisjointSet {
private:
  std::unordered_map<T, Node<T> *> _map;

public:
  DisjointSet() = default;

  ~DisjointSet() {
    for (auto &it : _map)
      delete it.second;
  }

  void makeSet(const T &data) {
    if (contains(data))
      return;
    auto node = new Node<T>(data, 0);
    node->_parent = node;
    _map[data] = node;
  }

  void doUnion(const T &data1, const T &data2) {
    Node<T> *node1 = _map.at(data1);
    Node<T> *node2 = _map.at(data2);

    Node<T> *parent1 = findSet(node1), *parent2 = findSet(node2);
    if (parent1->_val == parent2->_val)
      return;

    if (parent1->_rank >= parent2->_rank) {
      parent1->_rank += parent1->_rank == parent2->_rank;
      parent2->_parent = parent1;
    } else
      parent1->_parent = parent2;
  }

  T findSet(const T &data) { return findSet(_map.at(data))->_val; }

  size_t size() const { return _map.size(); }

  bool contains(const T &data) const { return _map.find(data) != _map.end(); }

  Node<T> *findSet(Node<T> *node) {
    if (node == node->_parent)
      return node;
    node->_parent = findSet(node->_parent);
    return node->_parent;
  }

  typename std::unordered_map<T, Node<T> *>::const_iterator begin() const {
    return _map.begin();
  }

  typename std::unordered_map<T, Node<T> *>::const_iterator end() const {
    return _map.end();
  }
};

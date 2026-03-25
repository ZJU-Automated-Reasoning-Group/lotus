// from https://github.com/can-lehmann/egraphs.cpp
// Copyright 2024 Can Joshua Lehmann
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef EGRAPHS_HPP
#define EGRAPHS_HPP

#include <cassert>
#include <cinttypes>
#include <deque>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define throw_error(Error, msg)                                                \
  {                                                                            \
    std::ostringstream message_stream;                                         \
    message_stream << msg;                                                     \
    throw Error(message_stream.str());                                         \
  }

#define owned(Type)                                                            \
  Type(const Type &other) = delete;                                            \
  Type &operator=(const Type &other) = delete;

namespace egraphs {
class Error : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

class ArenaAllocator {
private:
  struct Arena {
    static constexpr const uintptr_t SIZE = 4 * 1024 * 1024;

    uint8_t *data = nullptr;
    uintptr_t top = (uintptr_t)nullptr;

    explicit Arena() {
      data = new uint8_t[SIZE];
      top = (uintptr_t)data;
    }

    owned(Arena)

        Arena(Arena &&other)
        : data(std::exchange(other.data, nullptr)),
          top(std::exchange(other.top, 0)) {}

    ~Arena() {
      if (data != nullptr) {
        delete[] data;
        data = nullptr;
      }
    }

    void *alloc(uintptr_t size, uintptr_t alignment) {
      uintptr_t offset = top + alignment - top % alignment;
      if (offset + size > (uintptr_t)data + SIZE) {
        return nullptr;
      }
      top = offset + size;
      return (void *)offset;
    }
  };

  std::vector<Arena> _arenas;

public:
  ArenaAllocator() { _arenas.emplace_back(); }

  owned(ArenaAllocator)

      void *alloc(size_t size, size_t alignment) {
    assert(size < Arena::SIZE);
    void *ptr = _arenas.back().alloc(size, alignment);
    if (ptr == nullptr) {
      _arenas.emplace_back();
      ptr = _arenas.back().alloc(size, alignment);
      assert(ptr != nullptr);
    }
    return ptr;
  }

  template <class T> T *alloc() { return (T *)alloc(sizeof(T), alignof(T)); }
};

template <class NodeKind> class SimpleNodeData {
private:
  NodeKind _kind;

public:
  SimpleNodeData(const NodeKind &kind) : _kind(kind) {}

  NodeKind kind() const { return _kind; }

  bool operator==(const SimpleNodeData<NodeKind> &other) const {
    return _kind == other._kind;
  }

  bool operator!=(const SimpleNodeData<NodeKind> &other) const {
    return _kind != other._kind;
  }
};

} // namespace egraphs

template <class NodeKind> struct std::hash<egraphs::SimpleNodeData<NodeKind>> {
  size_t operator()(const egraphs::SimpleNodeData<NodeKind> &data) const {
    return std::hash<NodeKind>()(data.kind());
  }
};

namespace egraphs {
template <class NodeKind, class NodeData = SimpleNodeData<NodeKind>>
class EGraph {
public:
  struct Node;

private:
  template <class T> struct CycleRange {
    T *first = nullptr;
    T *last = nullptr;

    CycleRange() {}
    CycleRange(T *_first, T *_last) : first(_first), last(_last) {}

    bool empty() const { return first == nullptr; }
  };

  struct Use {
    Node *node = nullptr;
    size_t child_index = 0;
    Use *next = nullptr;

    Use(Node *_node, size_t _child_index)
        : node(_node), child_index(_child_index) {
      next = this;
    }
  };

  struct Down {
    Node *node = nullptr;
    Down *next = nullptr;

    Down(Node *_node) : node(_node) { next = this; }
  };

public:
  class EClass {
  public:
    class Iterator {
    public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = Node *;
      using difference_type = std::ptrdiff_t;
      using pointer = Node **;
      using reference = Node *&;

    private:
      Down *_initial = nullptr;
      Down *_current = nullptr;

      void next() {
        assert(_current != nullptr);
        if (_current->next == _initial) {
          _current = nullptr;
        } else {
          _current = _current->next;
        }
      }

      void skip_to_next_in_hashcons() {
        while (_current != nullptr && !_current->node->is_in_hashcons()) {
          next();
        }
      }

    public:
      explicit Iterator(Down *initial, Down *current)
          : _initial(initial), _current(current) {
        skip_to_next_in_hashcons();
      }

      Iterator &operator++() {
        if (_current != nullptr) {
          Down *prev = _current;
          next();
          skip_to_next_in_hashcons();
          if (_current == nullptr) {
            prev->next = _initial;
          } else {
            prev->next = _current;
          }
        }
        return *this;
      }

      Iterator &operator++(int) {
        Iterator old = *this;
        ++(*this);
        return old;
      }

      bool operator==(const Iterator &other) const {
        return _initial == other._initial && _current == other._current;
      }

      bool operator!=(const Iterator &other) const { return !(*this == other); }
      Node *operator*() const { return _current->node; }

      bool at_end() const { return _current == nullptr; }
    };

    class NodeDataMatcher {
    private:
      NodeData _data;

    public:
      NodeDataMatcher(const NodeData &data) : _data(data) {}

      bool matches(const Node *node) const { return _data == node->data(); }
    };

    class NodeKindMatcher {
    private:
      NodeKind _kind;

    public:
      NodeKindMatcher(const NodeKind &kind) : _kind(kind) {}

      bool matches(const Node *node) const {
        return _kind == node->data().kind();
      }
    };

    template <class Matcher> class MatchIterator {
    public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = Node *;
      using difference_type = std::ptrdiff_t;
      using pointer = Node **;
      using reference = Node *&;

    private:
      Iterator _iterator;
      Matcher _matcher;

      void skip_to_next_match() {
        while (!_iterator.at_end() && !_matcher.matches(*_iterator)) {
          ++_iterator;
        }
      }

    public:
      explicit MatchIterator(const Iterator &iterator, const Matcher &matcher)
          : _iterator(iterator), _matcher(matcher) {
        skip_to_next_match();
      }

      MatchIterator<Matcher> &operator++() {
        if (!_iterator.at_end()) {
          ++_iterator;
          skip_to_next_match();
        }
        return *this;
      }

      MatchIterator<Matcher> &operator++(int) {
        MatchIterator<Matcher> old = *this;
        ++(*this);
        return old;
      }

      bool operator==(const MatchIterator<Matcher> &other) const {
        return _iterator == other._iterator;
      }

      bool operator!=(const MatchIterator<Matcher> &other) const {
        return !(*this == other);
      }
      Node *operator*() const { return *_iterator; }
    };

    template <class Matcher> class MatchRange {
    private:
      EClass _e_class;
      Matcher _matcher;

    public:
      MatchRange(const EClass &e_class, const Matcher &matcher)
          : _e_class(e_class), _matcher(matcher) {}

      MatchIterator<Matcher> begin() {
        return MatchIterator<Matcher>(_e_class.begin(), _matcher);
      }
      MatchIterator<Matcher> end() {
        return MatchIterator<Matcher>(_e_class.end(), _matcher);
      }

      bool empty() { return begin() == end(); }
      bool not_empty() { return !empty(); }
    };

  private:
    Node *_root = nullptr;

  public:
    EClass(Node *node) : _root(node->root()) {}

    const Node *root() const { return _root; }

    Iterator begin() { return Iterator(_root->_down, _root->_down); }
    Iterator end() { return Iterator(_root->_down, nullptr); }

    template <class Matcher> MatchRange<Matcher> match(const Matcher &matcher) {
      return MatchRange<Matcher>(*this, matcher);
    }
    MatchRange<NodeDataMatcher> match(const NodeData &data) {
      return match(NodeDataMatcher(data));
    }
    MatchRange<NodeKindMatcher> match(const NodeKind &kind) {
      return match(NodeKindMatcher(kind));
    }
  };

  class Node {
  private:
    friend EGraph<NodeKind, NodeData>;

    NodeData _data;

    // Union Find
    size_t _rank = 0;
    Node *_up = nullptr;

    // If this node is a root node, uses contains a cyclic linked
    // list of all users of the node and down contains a cyclic
    // linked list of all nodes in the equivalence class.
    Use *_uses = nullptr;
    Down *_down;

    // Hashcons
    Node *_next_bucket = nullptr;
    Node **_prev_bucket = nullptr;

    // The children of the node are stored in a flexible array member
    // directly after this structure.
    size_t _child_count = 0;
    Node *_children[];

    Node(const NodeData &data, Down *down, size_t child_count, Node **children)
        : _data(data), _down(down), _child_count(child_count) {

      std::copy(children, children + child_count, _children);
    }

    bool is_in_hashcons() const { return _prev_bucket != nullptr; }

    void insert_uses(Use *uses) {
      if (_uses == nullptr) {
        _uses = uses;
      } else {
        Use *temp = _uses->next;
        _uses->next = uses->next;
        uses->next = temp;
      }
    }

    // Merges (union) equivalence classes.
    // `this` and `other` must both be distinct root nodes.
    // The rank of `this` must less or equal to the rank of `other`.
    CycleRange<Use> merge_roots(Node *other) {
      assert(this != other);
      assert(_rank <= other->_rank);
      assert(_up == nullptr);
      assert(other->_up == nullptr);

      _up = other;
      if (_rank == other->_rank) {
        other->_rank++;
      }

      Down *temp = other->_down->next;
      other->_down->next = _down->next;
      _down->next = temp;
      _down = nullptr;

      if (_uses != nullptr) {
        CycleRange<Use> range(_uses->next, _uses);
        other->insert_uses(_uses);
        _uses = nullptr;
        return range;
      } else {
        return CycleRange<Use>();
      }
    }

    // In order to find nodes in the hashcons, we need to hash
    // and compare nodes.

    static size_t hash(const NodeData &data, Node **children,
                       size_t child_count) {
      size_t hash = std::hash<NodeData>()(data);
      hash ^= std::hash<size_t>()(child_count) << 17;
      for (size_t it = 0; it < child_count; it++) {
        hash ^= std::hash<Node *>()(children[it]);
      }
      return hash;
    }

    size_t hash() const {
      return hash(_data, (Node **)_children, _child_count);
    }

    bool eq(const NodeData &data, Node **children, size_t child_count) const {
      if (_child_count != child_count || _data != data) {
        return false;
      }

      for (size_t it = 0; it < _child_count; it++) {
        if (_children[it] != children[it]) {
          return false;
        }
      }

      return true;
    }

  public:
    const NodeData &data() const { return _data; }

    EClass e_class() { return EClass(this); }

    Node **begin() { return _children; }
    Node **end() { return _children + _child_count; }
    size_t size() { return _child_count; }

    Node *at(size_t index) {
      if (index >= _child_count) {
        throw_error(Error, "");
      }
      return _children[index];
    }

    inline Node *operator[](size_t index) { return at(index); }

    // Finds the root node in the union find.
    // Performs path compression.
    Node *root() {
      Node *root = this;
      while (root->_up != nullptr) {
        root = root->_up;
      }
      Node *cur = this;
      while (cur->_up != nullptr) {
        Node *old_up = cur->_up;
        cur->_up = root;
        cur = old_up;
      }
      return root;
    }
  };

private:
  class Hashcons {
  private:
    Node **_data = nullptr;
    size_t _size = 0;

  public:
    Hashcons() {
      _size = 1024;
      _data = new Node *[_size]();
    }

    owned(Hashcons)

        ~Hashcons() {
      delete[] _data;
    }

    Node *get(const NodeData &data, Node **children, size_t child_count) {
      size_t hash = Node::hash(data, children, child_count) % _size;
      Node *node = _data[hash];
      while (node != nullptr) {
        if (node->eq(data, children, child_count)) {
          return node;
        }
        node = node->_next_bucket;
      }
      return nullptr;
    }

    Node *get(Node *node) {
      return get(node->_data, node->_children, node->_child_count);
    }

    // Removes a node from the hashcons.
    // Assumes that the node is currently in the hashcons.
    void erase(Node *node) {
      assert(node->is_in_hashcons());

      *node->_prev_bucket = node->_next_bucket;
      if (node->_next_bucket != nullptr) {
        node->_next_bucket->_prev_bucket = node->_prev_bucket;
      }
      node->_next_bucket = nullptr;
      node->_prev_bucket = nullptr;
    }

    // Assumes that node is not currently in the hashcons.
    void insert(Node *node) {
      assert(!node->is_in_hashcons());

      size_t hash = node->hash() % _size;
      node->_next_bucket = _data[hash];
      if (_data[hash] != nullptr) {
        _data[hash]->_prev_bucket = &node->_next_bucket;
      }
      _data[hash] = node;
      node->_prev_bucket = &_data[hash];
    }
  };

  Hashcons _hashcons;
  std::unordered_set<Node *> _roots;

  ArenaAllocator _node_allocator;
  ArenaAllocator _down_allocator;
  ArenaAllocator _use_allocator;

public:
  EGraph() {}
  owned(EGraph)

      const std::unordered_set<Node *> &roots() const {
    return _roots;
  }

  Node *node(const NodeData &data, Node **children, size_t child_count) {
    // All children must be root nodes
    for (size_t it = 0; it < child_count; it++) {
      assert(children[it]->_up == nullptr);
    }

    // Check if node is already in hashcons
    Node *node = _hashcons.get(data, children, child_count);
    if (node != nullptr) {
      return node->root();
    }

    // Node is not in hashcons -> allocate new node
    node = (Node *)_node_allocator.alloc(
        sizeof(Node) + sizeof(Node *) * child_count, alignof(Node));
    Down *down = _down_allocator.alloc<Down>();
    new (down) Down(node);
    new (node) Node(data, down, child_count, children);

    for (size_t it = 0; it < child_count; it++) {
      Use *use = _use_allocator.alloc<Use>();
      new (use) Use(node, it);
      children[it]->insert_uses(use);
    }

    // Insert into hashcons
    _hashcons.insert(node);
    _roots.insert(node);

    return node;
  }

  Node *node(const NodeData &data, const std::vector<Node *> &children) {
    return node(data, (Node **)(children.size() > 0 ? &children[0] : nullptr),
                children.size());
  }

  Node *node(const NodeData &data) { return node(data, nullptr, 0); }

  // Queue for storing merge operations before they are executed.
  // Will ignore already merged pairs.
  class MergeQueue {
  private:
    std::deque<std::pair<Node *, Node *>> _queue;

  public:
    MergeQueue() {}

    inline size_t size() const { return _queue.size(); }
    inline bool empty() const { return _queue.empty(); }

    std::pair<Node *, Node *> pop() {
      std::pair<Node *, Node *> pair = _queue.front();
      _queue.pop_front();
      return pair;
    }

    void merge_roots(Node *a, Node *b) {
      if (a != b) {
        _queue.emplace_back(a, b);
      }
    }

    void merge(Node *a, Node *b) { merge_roots(a->root(), b->root()); }
  };

  void merge(Node *a, Node *b) {
    MergeQueue queue;
    queue.merge(a, b);
    merge(queue);
  }

  bool merge(MergeQueue &queue) {
    bool changed = false;
    while (!queue.empty()) {
      std::pair<Node *, Node *> pair = queue.pop();
      Node *a = pair.first;
      Node *b = pair.second;

      a = a->root();
      b = b->root();
      if (a == b) {
        continue;
      }

      Node *root = b;
      Node *child = a;
      if (root->_rank < child->_rank) {
        std::swap(root, child);
      }

      CycleRange<Use> uses = child->merge_roots(root);
      changed = true;
      _roots.erase(child);

      // Update users
      if (!uses.empty()) {
        Use *use = uses.first;
        Use *prev = nullptr;
        while (true) {
          if (use->node->is_in_hashcons()) {
            _hashcons.erase(use->node);
            use->node->_children[use->child_index] = root;
            Node *other = _hashcons.get(use->node);
            if (other == nullptr) {
              _hashcons.insert(use->node);
            } else {
              queue.merge(use->node, other);
            }

            if (prev != nullptr) {
              prev->next = use;
            }
            prev = use;
          }

          if (use == uses.last) {
            break;
          }
          use = use->next;
        }
      }
    }

    return changed;
  }

  // Node cost type used for extraction.
  // Implements saturating arithmetic.
  class Cost {
  private:
    uint64_t _value = 0;

  public:
    Cost() {}
    Cost(uint64_t value) : _value(value) {}

    static inline Cost inf() { return Cost(~uint64_t(0)); }

    inline uint64_t value() const { return _value; }
    inline bool is_inf() const { return *this == Cost::inf(); }

    inline Cost operator+(const Cost &other) const {
      uint64_t res = 0;
      if (__builtin_add_overflow(_value, other._value, &res)) {
        return Cost::inf();
      } else {
        return Cost(res);
      }
    }

    inline Cost &operator+=(const Cost &other) {
      *this = *this + other;
      return *this;
    }

#define cmp(op)                                                                \
  inline bool operator op(const Cost &other) const {                           \
    return _value op other._value;                                             \
  }

    cmp(==) cmp(!=) cmp(<=) cmp(<) cmp(>=) cmp(>)

#undef cmp
  };

  // Mapping from root nodes to costs
  using Costs = std::unordered_map<Node *, Cost>;

  // Cost function for individual nodes.
  // The resulting cost must be greater than 0 and greater than
  // the cost of every child of the node.
  using CostFn = std::function<Cost(Node *node, const Costs &costs)>;

  // Cost function for node data.
  // Must return a cost greater than 0.
  using DataCostFn = std::function<Cost(const NodeData &)>;

  // Mapping from root nodes to the extracted representatives
  // from their equivalence class.
  using Extracted = std::unordered_map<Node *, Node *>;

  Extracted extract(const CostFn &cost_fn) {
    // Extraction is performed using dijkstra's algorithm
    // Starting at the leaf nodes, increasingly high cost nodes
    // are reached.

    struct QueueItem {
      Node *node = nullptr;
      Cost cost;

      QueueItem(Node *_node, Cost _cost) : node(_node), cost(_cost) {}

      bool operator<(const QueueItem &other) const { return cost > other.cost; }
    };

    Extracted extracted;
    Costs costs;
    std::priority_queue<QueueItem> queue;

    for (Node *root : _roots) {
      extracted.insert({root, root});
      costs.insert({root, Cost::inf()});
    }

    // Insert leaf nodes
    for (Node *root : _roots) {
      for (Node *node : root->e_class()) {
        if (node->size() == 0) {
          Cost cost = cost_fn(node, costs);
          if (cost < costs.at(root)) {
            queue.push(QueueItem(root, cost));
            extracted.at(root) = node;
            costs.at(root) = cost;
          }
        }
      }
    }

    while (!queue.empty()) {
      QueueItem item = queue.top();
      queue.pop();

      if (item.cost != costs.at(item.node)) {
        // Already found a lower cost representative for this equivalence class
        continue;
      }

      if (item.node->_uses != nullptr) {
        Use *use = item.node->_uses;
        while (true) {
          Node *node = use->node;
          if (node->is_in_hashcons()) {
            Cost cost = cost_fn(node, costs);
            assert(cost > item.cost);
            Node *root = node->root();
            if (cost < costs.at(root)) {
              queue.push(QueueItem(root, cost));
              extracted.at(root) = node;
              costs.at(root) = cost;
            }
          }

          if (use->next == item.node->_uses) {
            break;
          }
          use = use->next;
        }
      }
    }

    return extracted;
  }

  Extracted extract(const DataCostFn &data_cost) {
    return extract([&](Node *node, const Costs &costs) {
      Cost cost = data_cost(node->data());
      for (Node *child : *node) {
        cost += costs.at(child);
      }
      return cost;
    });
  }

  Extracted extract() {
    return extract([&](const NodeData &data) { return 1; });
  }

  void write_dot(std::ostream &stream) const {
    std::unordered_map<Node *, size_t> ids;

    stream << "digraph {\n";
    stream << "compound=true;\n";

    for (Node *root : _roots) {
      for (Node *node : root->e_class()) {
        ids.insert({node, ids.size()});
      }

      stream << "subgraph cluster" << ids.at(root) << " {\n";
      for (Node *node : root->e_class()) {
        stream << "node" << ids.at(node) << " [label=\"" << node->data()
               << "\"]\n";
      }
      stream << "}\n";
    }

    for (Node *root : _roots) {
      for (Node *node : root->e_class()) {
        for (Node *child : *node) {
          stream << "node" << ids.at(node) << " -> node" << ids.at(child);
          // stream << " [lhead=cluster" << ids.at(child->root()) << "]";
          stream << ";\n";
        }
      }
    }

    stream << "}";
  }

  void save_dot(const char *path) {
    std::ofstream stream;
    stream.open(path);
    if (!stream) {
      throw_error(Error, "Failed to open " << path);
    }
    write_dot(stream);
  }

  void write_dot(std::ostream &stream, const Extracted &extracted,
                 Node *extraction_root) const {
    std::unordered_map<Node *, size_t> ids;

    stream << "digraph {\n";
    stream << "compound=true;\n";

    extraction_root = extraction_root->root();

    std::deque<Node *> queue;
    queue.push_back(extraction_root);
    ids.insert({extraction_root, ids.size()});

    while (!queue.empty()) {
      Node *root = queue.front();
      queue.pop_front();
      stream << "node" << ids.at(root) << " [label=\""
             << extracted.at(root)->data() << "\"]\n";

      for (Node *child : *extracted.at(root)) {
        if (ids.find(child) == ids.end()) {
          ids.insert({child, ids.size()});
          queue.push_back(child);
        }
      }
    }

    for (const auto &pair : ids) {
      Node *root = pair.first;
      size_t id = pair.second;
      for (Node *child : *extracted.at(root)) {
        stream << "node" << id << " -> node" << ids.at(child) << ";\n";
      }
    }

    stream << "}";
  }

  void save_dot(const char *path, const Extracted &extracted,
                Node *extraction_root) {
    std::ofstream stream;
    stream.open(path);
    if (!stream) {
      throw_error(Error, "Failed to open " << path);
    }
    write_dot(stream, extracted, extraction_root);
  }
};
} // namespace egraphs

#undef owned
#undef throw_error

#endif

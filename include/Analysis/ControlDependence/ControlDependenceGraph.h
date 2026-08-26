// Generic graph shared by the control-dependence algorithms and adapters.
#pragma once

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lotus::cd::detail {

class GraphNode {
public:
  unsigned getID() const { return m_id; }

  const std::vector<GraphNode *> &successors() const { return m_successors; }
  const std::vector<GraphNode *> &predecessors() const {
    return m_predecessors;
  }

private:
  friend class Graph;
  explicit GraphNode(unsigned id) : m_id(id) {}

  unsigned m_id;
  std::vector<GraphNode *> m_successors;
  std::vector<GraphNode *> m_predecessors;
};

class Graph {
  using NodeStorage = std::vector<std::unique_ptr<GraphNode>>;

public:
  class NodeIterator {
  public:
    explicit NodeIterator(NodeStorage::const_iterator iterator)
        : m_iterator(iterator) {}

    GraphNode *operator*() const { return m_iterator->get(); }
    NodeIterator &operator++() {
      ++m_iterator;
      return *this;
    }
    bool operator!=(const NodeIterator &other) const {
      return m_iterator != other.m_iterator;
    }

  private:
    NodeStorage::const_iterator m_iterator;
  };

  class NodeRange {
  public:
    explicit NodeRange(const NodeStorage &nodes) : m_nodes(nodes) {}
    NodeIterator begin() const { return NodeIterator(m_nodes.begin()); }
    NodeIterator end() const { return NodeIterator(m_nodes.end()); }

  private:
    const NodeStorage &m_nodes;
  };

  Graph() = default;
  explicit Graph(std::string name) : m_name(std::move(name)) {}

  Graph(const Graph &) = delete;
  Graph &operator=(const Graph &) = delete;
  Graph(Graph &&) noexcept = default;
  Graph &operator=(Graph &&) noexcept = default;

  GraphNode &createNode() {
    std::unique_ptr<GraphNode> node(new GraphNode(m_nodes.size() + 1));
    GraphNode &result = *node;
    m_nodes.push_back(std::move(node));
    return result;
  }

  void addEdge(GraphNode &source, GraphNode &target) {
    if (std::find(source.m_successors.begin(), source.m_successors.end(),
                  &target) != source.m_successors.end())
      return;
    source.m_successors.push_back(&target);
    target.m_predecessors.push_back(&source);
    if (source.m_successors.size() == 2)
      m_predicates.push_back(&source);
  }

  GraphNode *getNode(unsigned id) {
    assert(id > 0 && id <= m_nodes.size());
    return m_nodes[id - 1].get();
  }
  const GraphNode *getNode(unsigned id) const {
    assert(id > 0 && id <= m_nodes.size());
    return m_nodes[id - 1].get();
  }

  NodeRange nodes() const { return NodeRange(m_nodes); }

  const std::vector<GraphNode *> &predicates() const { return m_predicates; }
  bool isPredicate(const GraphNode &node) const {
    return node.successors().size() > 1;
  }
  size_t size() const { return m_nodes.size(); }
  bool empty() const { return m_nodes.empty(); }
  const std::string &getName() const { return m_name; }

private:
  std::string m_name;
  NodeStorage m_nodes;
  std::vector<GraphNode *> m_predicates;
};

using NodeSet = std::set<GraphNode *>;
using DependenceMap = std::map<GraphNode *, NodeSet>;
using DependenceResult = std::pair<DependenceMap, DependenceMap>;

} // namespace lotus::cd::detail

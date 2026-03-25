/**
 * @file GraphTraits.h
 * @brief Graph-traits infrastructure for Andersen's constraint graph.
 *
 * This file provides two building blocks used throughout SparrowAA:
 *
 *  1. **`MapValueIterator`** — a generic iterator adapter that wraps a
 *     `std::map` / `std::unordered_map` iterator and exposes only the
 *     *value* part of each key-value pair.  This lets graph algorithms
 *     iterate over nodes without caring about the internal key type.
 *
 *  2. **`AndersGraphTraits<GraphType>`** — a traits class (similar to
 *     `llvm::GraphTraits`) that must be specialised for each concrete graph
 *     type used in the analysis.  Specialisations provide:
 *     - `NodeType` — the node class.
 *     - `NodeIterator` / `ChildIterator` — iterator types.
 *     - `node_begin` / `node_end` — iterate over all nodes.
 *     - `child_begin` / `child_end` — iterate over successors of a node.
 *
 *  The `SparseBitVectorGraph` specialisation is provided in
 *  `SparseBitVectorGraph.h`.  The `CycleDetector` template uses
 *  `AndersGraphTraits` to remain graph-type-agnostic.
 */

#ifndef ANDERSEN_GRAPHTRAITS_H
#define ANDERSEN_GRAPHTRAITS_H

/**
 * @class MapValueIterator
 * @brief Iterator adapter that exposes only the value part of a map iterator.
 *
 * Wraps a `MapIterator` (e.g., `std::map<K,V>::iterator`) and dereferences
 * to the `V` (value) part, discarding the key.  This is useful when a graph
 * stores its nodes as values in a map keyed by node index, and callers want
 * to iterate over nodes without dealing with the key.
 *
 * @tparam MapIterator  An iterator over a map whose `value_type` is a
 *                      `std::pair<Key, Value>`.
 */
template <class MapIterator> class MapValueIterator {
private:
  MapIterator itr;
  typedef typename MapIterator::value_type::second_type MapValueType;

public:
  explicit MapValueIterator(const MapIterator &i) : itr(i) {}

  bool operator==(const MapValueIterator &other) { return itr == other.itr; }
  bool operator!=(const MapValueIterator &other) { return !(*this == other); }

  const MapValueType &operator*() { return itr->second; }
  const MapValueType &operator*() const { return itr->second; }

  const MapValueType *operator->() const { return &(itr->second); }

  // Pre-increment
  MapValueIterator &operator++() {
    ++itr;
    return *this;
  }
  // Post-increment
  const MapValueIterator operator++(int) {
    MapValueIterator ret(itr);
    ++itr;
    return ret;
  }
};

/**
 * @class AndersGraphTraits
 * @brief Traits class for graph types used in Andersen's analysis.
 *
 * This is the primary customisation point for graph algorithms in SparrowAA
 * (in particular `CycleDetector`).  The default template is intentionally
 * empty; each concrete graph type must provide a full specialisation.
 *
 * A valid specialisation must define:
 *
 * ```cpp
 * template <> class AndersGraphTraits<MyGraph> {
 *   using NodeType     = ...;  // Node class
 *   using NodeIterator = ...;  // Iterator over all nodes
 *   using ChildIterator = ...; // Iterator over successors of a node
 *
 *   static ChildIterator child_begin(NodeType *n);
 *   static ChildIterator child_end(NodeType *n);
 *
 *   static NodeIterator node_begin(MyGraph *g);
 *   static NodeIterator node_end(MyGraph *g);
 * };
 * ```
 *
 * The `SparseBitVectorGraph` specialisation is provided at the bottom of
 * `SparseBitVectorGraph.h`.
 *
 * @tparam GraphType  The concrete graph type being specialised.
 */
template <class GraphType> class AndersGraphTraits {
  // Specialisations must provide:
  //   typedef NodeType           - Type of Node in the graph
  //   typedef NodeIterator       - Type used to iterate over nodes in graph
  //   typedef ChildIterator      - Type used to iterate over children in graph
  //   static ChildIterator child_begin(NodeType*)
  //   static ChildIterator child_end(NodeType*)
  //   static NodeIterator node_begin(const GraphType*)
  //   static NodeIterator node_end(const GraphType*)
};

#endif

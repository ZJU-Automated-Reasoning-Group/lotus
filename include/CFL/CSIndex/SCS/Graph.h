#pragma once

#include "CFL/CSIndex/FLARE/Graph.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace lotus::cfl::cs_index::scs {

struct Edge {
  int id = -1;
  int source = -1;
  int target = -1;
  int structural_label = 0;
  int event_label = 0;
};

struct Vertex {
  int id = -1;
  int func_id = -1;
};

/**
 * A program graph whose edges carry independent structural and security-event
 * projections. Edge identities allow parallel edges to retain distinct roles.
 */
class Graph {
public:
  using EventLabelMap = std::map<std::pair<int, int>, int>;

  void addVertex(int id, int func_id = -1);
  int addEdge(int source, int target, int structural_label = 0,
              int event_label = 0);
  int addEventPredecessor(int vertex, int event_label);
  int addEventSuccessor(int vertex, int event_label);

  bool hasVertex(int id) const;
  int num_vertices() const;
  int num_edges() const;

  const Vertex &vertex(int id) const;
  const Edge &edge(int edge_id) const;
  const std::vector<int> &out_edges(int vertex_id) const;
  const std::vector<Edge> &edges() const;
  std::set<int> observedEvents() const;

  /**
   * Adapt an existing CSIndex graph. Because Graph labels are keyed by endpoint
   * pair, this adapter cannot recover distinct labels for parallel edges.
   */
  static Graph fromGraph(flare::Graph &graph,
                         const EventLabelMap &event_labels = {});

private:
  std::vector<Vertex> vertices_;
  std::vector<Edge> edges_;
  std::vector<std::vector<int>> out_edges_;
};

} // namespace lotus::cfl::cs_index::scs

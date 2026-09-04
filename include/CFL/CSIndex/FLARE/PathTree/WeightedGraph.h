#ifndef _DWGRAPH_H
#define _DWGRAPH_H

#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <vector>
// #include <algorithm>
#include <utility>
// #include <cmath>
#include <unordered_map>

namespace lotus::cfl::cs_index::flare::path_tree {

using namespace std;

#ifndef MAX_VAL
#define MAX_VAL 15000000
#endif
#ifndef MIN_VAL
#define MIN_VAL -15000000
#endif

/**
 * @brief Vertex structure for directed weighted graphs.
 */
struct DWVertex {
  int id;         ///< Unique vertex identifier
  bool visited;   ///< Visited flag for traversal algorithms
  int pre_order;  ///< Pre-order number for path tree labeling
  int post_order; ///< Post-order number for path tree labeling
};

/**
 * @brief Edge properties for directed weighted graphs.
 */
struct DWEdgeProp {
  int src;    ///< Source vertex ID
  int trg;    ///< Target vertex ID
  int weight; ///< Weight of the edge
};

/**
 * @brief Vertex properties for directed weighted graphs.
 */
struct DWVertexProp {
  int id;     ///< Vertex identifier
  int weight; ///< Vertex weight
  int edgeid; ///< Edge identifier
};

using DWEdgeList = list<int>; ///< Edge list represented by edge ID list
using DWVertexList = unordered_map<int, DWVertex>; ///< Vertices list storing real vertex properties
using DWEdgeOpMap = unordered_map<int, DWEdgeProp>; ///< Edge properties map
using Edge = pair<int, DWVertexProp>;               ///< Edge representation

/**
 * @brief Less-than comparator for edges.
 */
struct ltEdge {
  /**
   * @brief Compare two edges for ordering.
   * @param e1 First edge
   * @param e2 Second edge
   * @return true if e1 < e2, false otherwise
   */
  bool operator()(const Edge &e1, const Edge &e2) const {
    if (e1.first < e2.first)
      return true;
    if (e1.first == e2.first && e1.second.id < e2.second.id)
      return true;
    if (e1.first == e2.first && e1.second.id == e2.second.id &&
        e1.second.weight < e2.second.weight)
      return true;
    if (e1.first == e2.first && e1.second.id == e2.second.id &&
        e1.second.weight == e2.second.weight &&
        e1.second.edgeid < e2.second.edgeid)
      return true;
    return false;
  }
};
using EdgeMap = map<Edge, Edge, ltEdge>; ///< Edge map with custom ordering

/**
 * @brief Structure containing incoming and outgoing edge lists for a vertex.
 */
struct DWIn_OutList {
  DWEdgeList inList;  ///< List of incoming edge IDs
  DWEdgeList outList; ///< List of outgoing edge IDs
};
using DWGRA = map<int, DWIn_OutList>; ///< Directed weighted graph representation

/**
 * @brief Directed weighted graph class with edge and vertex management.
 *
 * This class provides a comprehensive representation for directed weighted
 * graphs with support for edge weights, vertex properties, and various graph
 * operations.
 */
class WeightedGraph {
public:
  DWGRA graph;           ///< Graph adjacency structure
  DWVertexList vl;       ///< Vertex list
  DWEdgeOpMap edgeOpMap; ///< Edge properties map

  int maxEdgeId; ///< Maximum edge ID used

  /**
   * @brief Read graph from input stream.
   * @param is Input stream containing graph data
   */
  void readGraph(istream &is);

  /**
   * @brief Read graph from input stream (alternative format).
   * @param is Input stream containing graph data
   */
  void readGraph1(istream &is);

  /**
   * @brief Write graph to output stream.
   * @param os Output stream to write graph data
   */
  void writeGraph(ostream &os);

public:
  /**
   * @brief Default constructor.
   */
  WeightedGraph();

  /**
   * @brief Constructor with existing graph data.
   * @param gra Graph adjacency structure
   * @param vl Vertex list
   */
  WeightedGraph(DWGRA &gra, DWVertexList &vl);

  /**
   * @brief Constructor that reads graph from input stream.
   * @param is Input stream containing graph data
   */
  WeightedGraph(istream &is);

  /**
   * @brief Destructor.
   */
  ~WeightedGraph();

  /**
   * @brief Print graph structure to standard output.
   */
  void printGraph();

  /**
   * @brief Add a vertex to the graph.
   * @param id Vertex identifier
   */
  void addVertex(int id);

  /**
   * @brief Remove a vertex from the graph.
   * @param id Vertex identifier to remove
   */
  void removeVertex(int id);

  /**
   * @brief Remove an edge between two vertices.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   */
  void removeEdge(int src, int dst);

  /**
   * @brief Remove an edge with specific ID.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param eid Edge ID
   */
  void removeEdgeWithID(int src, int dst, int eid);

  /**
   * @brief Remove an edge with specific weight.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param weight Edge weight
   */
  void removeEdgeWithWeight(int src, int dst, int weight);

  /**
   * @brief Add an edge with ID and weight.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param eid Edge ID
   * @param weight Edge weight
   */
  void addEdge(int src, int dst, int eid, int weight);

  /**
   * @brief Add an edge with weight.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param weight Edge weight
   */
  void addEdgeWithWeight(int src, int dst, int weight);

  /**
   * @brief Update an existing edge.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param weight New edge weight
   */
  void updateEdge(int src, int dst, int weight);

  /**
   * @brief Get the number of vertices.
   * @return Number of vertices
   */
  int num_vertices();

  /**
   * @brief Get the number of edges.
   * @return Number of edges
   */
  int num_edges();

  /**
   * @brief Get reference to vertex list.
   * @return Reference to vertex list
   */
  DWVertexList &vertices();

  /**
   * @brief Get outgoing edges for a vertex.
   * @param vid Vertex ID
   * @return Reference to outgoing edge list
   */
  DWEdgeList &out_edges(int vid);

  /**
   * @brief Get incoming edges for a vertex.
   * @param vid Vertex ID
   * @return Reference to incoming edge list
   */
  DWEdgeList &in_edges(int vid);

  /**
   * @brief Get out-degree of a vertex.
   * @param vid Vertex ID
   * @return Out-degree count
   */
  int out_degree(int vid);

  /**
   * @brief Get in-degree of a vertex.
   * @param vid Vertex ID
   * @return In-degree count
   */
  int in_degree(int vid);

  /**
   * @brief Get weight of edge between two vertices (for Edmonds' algorithm).
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @return Edge weight
   */
  int weight(int src, int dst);

  /**
   * @brief Get edge ID between two vertices (for Edmonds' algorithm).
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @return Edge ID
   */
  int edgeId(int src, int dst);

  /**
   * @brief Get edge properties between two vertices (for Edmonds' algorithm).
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @return Edge properties
   */
  DWVertexProp edge(int src, int dst);

  /**
   * @brief Remove vertex from vertex list (for Edmonds' algorithm).
   * @param id Vertex ID to remove
   */
  void removeVertexfromVL(int id);

  /**
   * @brief Get maximum vertex ID.
   * @return Maximum vertex ID
   */
  int maxid();

  /**
   * @brief Get root vertices (vertices with no incoming edges).
   * @return Set of root vertex IDs
   */
  set<int> getRoots();

  /**
   * @brief Check if vertex exists.
   * @param id Vertex ID
   * @return true if vertex exists, false otherwise
   */
  bool hasVertex(int id);

  /**
   * @brief Check if edge exists between two vertices.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @return true if edge exists, false otherwise
   */
  bool hasEdge(int src, int dst);

  /**
   * @brief Check if edge with specific ID exists.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param eid Edge ID
   * @return true if edge exists, false otherwise
   */
  bool hasEdgeWithID(int src, int dst, int eid);

  /**
   * @brief Assignment operator.
   * @param other Graph to copy from
   * @return Reference to this graph
   */
  WeightedGraph &operator=(const WeightedGraph &other);

  /**
   * @brief Access vertex by ID.
   * @param id Vertex ID
   * @return Reference to vertex
   */
  DWVertex &operator[](const int id);

  /**
   * @brief Clear all vertices and edges.
   */
  void clear();

  /**
   * @brief Trim whitespace from string.
   * @param str String to trim
   */
  void strTrim(string &str);

  /**
   * @brief Export graph to GDL format.
   * @param out Output stream
   */
  void toGDL(ostream &out);

  // Dec 15
  // avoid direct operation on edgeOpMap
  /**
   * @brief Get weight of edge by edge ID.
   * @param eid Edge ID
   * @return Edge weight
   */
  int weight(int eid);

  /**
   * @brief Get source vertex of edge by edge ID.
   * @param eid Edge ID
   * @return Source vertex ID
   */
  int source(int eid);

  /**
   * @brief Get target vertex of edge by edge ID.
   * @param eid Edge ID
   * @return Target vertex ID
   */
  int target(int eid);

  // remove edges with id
  /**
   * @brief Remove edge by edge ID.
   * @param eid Edge ID to remove
   */
  void removeEdge(int eid);
};


} // namespace lotus::cfl::cs_index::flare::path_tree

#endif

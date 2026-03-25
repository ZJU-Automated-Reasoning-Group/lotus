#ifndef _GRAPH_H
#define _GRAPH_H

#include "CFL/CSIndex/BitVector.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

#ifndef MAX_VAL
#define MAX_VAL 100000000
#endif
#ifndef MIN_VAL
#define MIN_VAL -100000000
#endif

/**
 * @brief Node types for different vertex classifications.
 */
enum NodeType {
  NORMAL = 0, ///< Regular node
  INPUT,      ///< Input node
  ARG,        ///< Argument node
  RET,        ///< Return node
  OUTPUT      ///< Output node
};

/**
 * @brief Represents a vertex in the graph with various properties for analysis.
 */
struct Vertex {
  int id;                      ///< Unique vertex identifier
  bool visited;                ///< Visited flag for traversal algorithms
  int min_parent_level;        ///< Minimum parent level in hierarchy
  bool fat;                    ///< Flag indicating if this is a fat node
  int topo_id;                 ///< Topological ordering ID
  int top_level;               ///< Topological level
  int path_id;                 ///< Path identifier
  int dfs_order;               ///< DFS traversal order
  int pre_order;               ///< Pre-order traversal number
  int post_order;              ///< Post-order traversal number
  int first_visit;             ///< First visit timestamp (for testing)
  int kind = NodeType::NORMAL; ///< Node type classification
  int func_id = -1;            ///< Function identifier
  int o_vid = -1;              ///< Original vertex ID
  bool removed = false;        ///< Removal flag

  double tcs;          ///< Total coverage score
  int mingap;          ///< Minimum gap value
  vector<int> *pre;    ///< Predecessor list
  vector<int> *post;   ///< Successor list
  vector<int> *middle; ///< Middle nodes list

  /**
   * @brief Constructor with vertex ID.
   * @param ID Unique identifier for the vertex
   */
  Vertex(int ID) : id(ID) {
    top_level = -1;
    visited = false;
  }

  /**
   * @brief Default constructor.
   */
  Vertex() {
    top_level = -1;
    visited = false;
  };
};

typedef vector<int> EdgeList; ///< Edge list represented by vertex ID list
typedef vector<Vertex>
    VertexList; ///< Vertices list storing real vertex properties

/**
 * @brief Structure containing incoming and outgoing edge lists for a vertex.
 */
struct In_OutList {
  EdgeList inList;  ///< List of incoming edge vertex IDs
  EdgeList outList; ///< List of outgoing edge vertex IDs
};
typedef vector<In_OutList> GRA; ///< Graph representation as adjacency list

/**
 * @brief Hash function for std::pair<int, int> used in unordered_map.
 */
struct pair_hash {
  /**
   * @brief Hash operator for integer pairs.
   * @param p Pair of integers to hash
   * @return Hash value for the pair
   */
  std::size_t operator()(const std::pair<int, int> &p) const {
    long p1 = p.first;
    long p2 = p.second;
    if (p1 < 0 || p2 < 0) {
      cerr << "key error " << p1 << " " << p2 << "\n";
      exit(10);
    }
    long x = (p1 << 32) | p2;
    return std::hash<long>{}(x);
  }
};

/**
 * @brief Main graph class for representing directed graphs with various
 * analysis capabilities.
 *
 * This class provides a comprehensive graph representation with support for:
 * - Vertex and edge management
 * - Reachability queries
 * - Labeling and indexing
 * - Summary edge computation
 */
class Graph {
protected:
  VertexList vl;      ///< List of vertices
  GRA graph;          ///< Adjacency list representation
  int n_vertices = 0; ///< Number of vertices
  int n_edges = 0;    ///< Number of edges

  std::unordered_map<std::pair<int, int>, int, pair_hash>
      pos_label_map; ///< Positive label mapping
  std::unordered_map<std::pair<int, int>, int, pair_hash>
      neg_label_map; ///< Negative label mapping
  std::unordered_map<int, std::set<int>>
      summary_edges; ///< Summary edges (out <- in, reversed map)

public:
  /**
   * @brief Default constructor.
   */
  Graph();

  /**
   * @brief Constructor with initial vertex count.
   * @param n Initial number of vertices
   */
  explicit Graph(int n);

  /**
   * @brief Constructor that reads graph from input stream.
   * @param is Input stream containing graph data
   */
  explicit Graph(istream &is);

  /**
   * @brief Constructor with existing graph data.
   * @param gra Adjacency list representation
   * @param vl Vertex list
   */
  Graph(GRA &gra, VertexList &vl);

  /**
   * @brief Destructor.
   */
  ~Graph();

  /**
   * @brief Read graph from input stream.
   * @param is Input stream containing graph data
   */
  void readGraph(istream &is);

  /**
   * @brief Write graph to output stream.
   * @param os Output stream to write graph data
   */
  void writeGraph(ostream &os);

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
  virtual void remove_vertex(int id);

  /**
   * @brief Add an edge between two vertices.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   */
  void addEdge(int src, int dst);

  /**
   * @brief Add an edge with label between two vertices.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @param label Edge label
   */
  void addEdge(int src, int dst, int label);

  /**
   * @brief Get the number of vertices in the graph.
   * @return Number of vertices
   */
  int num_vertices();

  /**
   * @brief Get the number of edges in the graph.
   * @return Number of edges
   */
  int num_edges();

  /**
   * @brief Get reference to the vertex list.
   * @return Reference to vertex list
   */
  VertexList &vertices();

  /**
   * @brief Get outgoing edges for a vertex.
   * @param vid Vertex ID
   * @return Reference to outgoing edge list
   */
  EdgeList &out_edges(int vid);

  /**
   * @brief Get incoming edges for a vertex.
   * @param vid Vertex ID
   * @return Reference to incoming edge list
   */
  EdgeList &in_edges(int vid);

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
   * @brief Get root vertices (vertices with no incoming edges).
   * @return Vector of root vertex IDs
   */
  vector<int> getRoots();

  /**
   * @brief Check if an edge exists between two vertices.
   * @param src Source vertex ID
   * @param dst Destination vertex ID
   * @return true if edge exists, false otherwise
   */
  bool hasEdge(int src, int dst);

  /**
   * @brief Assignment operator.
   * @param other Graph to copy from
   * @return Reference to this graph
   */
  Graph &operator=(const Graph &other);

  /**
   * @brief Access vertex by ID.
   * @param id Vertex ID
   * @return Reference to vertex
   */
  Vertex &operator[](int id);

  /**
   * @brief Access vertex by ID with bounds checking.
   * @param id Vertex ID
   * @return Reference to vertex
   */
  Vertex &at(int id);

  /**
   * @brief Clear all vertices and edges from the graph.
   */
  void clear();

  /**
   * @brief Trim whitespace from the right side of a string.
   * @param str String to trim
   */
  void strTrimRight(string &str);

  /**
   * @brief Constructor from adjacency lists.
   * @param inlist Map of incoming edges
   * @param outlist Map of outgoing edges
   */
  Graph(unordered_map<int, vector<int>> &inlist,
        unordered_map<int, vector<int>> &outlist);

  /**
   * @brief Extract adjacency lists from the graph.
   * @param inlist Map to store incoming edges
   * @param outlist Map to store outgoing edges
   */
  void extract(unordered_map<int, vector<int>> &inlist,
               unordered_map<int, vector<int>> &outlist);

  /**
   * @brief Print adjacency list maps.
   * @param inlist Map of incoming edges
   * @param outlist Map of outgoing edges
   */
  void printMap(unordered_map<int, vector<int>> &inlist,
                unordered_map<int, vector<int>> &outlist);

  /**
   * @brief Print all edges in the graph.
   */
  void print_edges();

  /**
   * @brief Get total coverage score for a vertex.
   * @param vid Vertex ID
   * @return Total coverage score
   */
  double tcs(int vid);

  /**
   * @brief Sort edges in adjacency lists.
   */
  void sortEdges();

  /**
   * @brief Split string by delimiter.
   * @param s String to split
   * @param delim Delimiter character
   * @return Vector of split strings
   */
  static vector<string> split(const string &s, char delim);

  /**
   * @brief Split string by delimiter into existing vector.
   * @param s String to split
   * @param delim Delimiter character
   * @param elems Vector to store results
   * @return Reference to elems vector
   */
  static vector<string> &split(const string &s, char delim,
                               vector<string> &elems);

  /**
   * @brief Build summary edges for reachability analysis.
   */
  void build_summary_edges();

  /**
   * @brief Get the number of summary edges.
   * @return Number of summary edges
   */
  size_t summary_edge_size();

  /**
   * @brief Convert graph to indexing format.
   */
  void to_indexing_graph();

  /**
   * @brief Remove an edge between two vertices.
   * @param s Source vertex ID
   * @param t Target vertex ID
   */
  void removeEdge(int s, int t);

  /**
   * @brief Check graph consistency and validity.
   */
  void check();

  /**
   * @brief Get label for edge between two vertices.
   * @param s Source vertex ID
   * @param t Target vertex ID
   * @return Edge label
   */
  int label(int s, int t);

  /**
   * @brief Add summary edges to the graph.
   */
  void add_summary_edges();
};

#endif

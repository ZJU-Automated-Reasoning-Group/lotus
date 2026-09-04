#ifndef _QUERY_H_
#define _QUERY_H_

#include "CFL/CSIndex/FLARE/ReachabilityQuery.h"
#include "CFL/CSIndex/FLARE/GraphAlgorithms.h"

namespace lotus::cfl::cs_index::flare {

#define COMPACTVECTOR

using namespace std;

/**
 * @brief Main query class implementing various reachability algorithms.
 *
 * This class provides implementations of different reachability query methods
 * including GRAIL labeling, path tree indexing, and materialization techniques.
 */
class Index : public ReachabilityQuery {
public:
  vector<vector<pair<int, int>>>
      graillabels;   ///< Multiple random labeling for GRAIL algorithm
  int grail_dim = 5; ///< Dimension for GRAIL labeling
protected:
  Graph &g;              ///< Reference to the main graph
  Graph gategraph;       ///< Gate graph for indexing
  BitVector *gates;     ///< Bit vector for gate information
  map<int, int> gatemap; ///< Map from original graph node ID to gate graph ID
  vector<vector<int>> labels, lin, lout; ///< Labeling structures for indexing
  string filestemstr; ///< File stem string for I/O operations
  int radius, gsize, gatesize, gateedgesize, labeltype, indextype, dim,
      num_bits, epsilon, visitnum; ///< Various parameters
  int ref, QueryCnt;               ///< Reference count and query counter
  double preselectratio;           ///< Pre-selection ratio for optimization

  string method_name; ///< Name of the query method
  bool useLocalGates, usePartialLocalGates, useMultiLabels, useTopoOrder,
      useGlobalMultiLabels; ///< Feature flags
  // improvement options
  vector<int> topoid, que, dist, visited; ///< Auxiliary vectors for algorithms
  vector<vector<vector<int>>> localgates; ///< Local gates for optimization

  // for materialization
  bool ismaterialized; ///< Flag indicating if materialization is used
  vector<vector<vector<int>>>
      inoutgates;               ///< Input/output gates for materialization
  vector<BitVector *> inneigs; ///< Bit vectors for incoming neighbors
  BitVector *materialized;     ///< Materialized data structure

  // for stat computing time
  int reachtime; ///< Time spent on reachability queries

public:
  /**
   * @brief Default constructor.
   */
  Index();

  /**
   * @brief Constructor with graph file.
   * @param grafile Path to graph file
   */
  Index(const char *grafile);

  /**
   * @brief Constructor with file stem, graph file, and radius.
   * @param filestem File stem for I/O operations
   * @param grafile Path to graph file
   * @param _r Radius parameter
   */
  Index(const char *filestem, const char *grafile, int _r);

  /**
   * @brief Constructor with file stem, graph file, radius, pre-selection ratio,
   * and materialization flag.
   * @param filestem File stem for I/O operations
   * @param grafile Path to graph file
   * @param _r Radius parameter
   * @param _ps Pre-selection ratio
   * @param mat Materialization flag
   */
  Index(const char *filestem, const char *grafile, int _r, double _ps,
        bool mat);

  /**
   * @brief Constructor with file stem, graph reference, radius, pre-selection
   * ratio, and materialization flag.
   * @param filestem File stem for I/O operations
   * @param ig Reference to graph
   * @param _r Radius parameter
   * @param _ps Pre-selection ratio
   * @param mat Materialization flag
   */
  Index(const char *filestem, Graph &ig, int _r, double _ps, bool mat);

  /**
   * @brief Constructor with multiple file inputs.
   * @param gatefile Path to gate file
   * @param ggfile Path to gate graph file
   * @param indexfile Path to index file
   * @param grafile Path to graph file
   */
  Index(const char *gatefile, const char *ggfile, const char *indexfile,
        const char *grafile);

  /**
   * @brief Virtual destructor.
   */
  virtual ~Index();

  /**
   * @brief Initialize feature flags.
   */
  void initFlags();

  /**
   * @brief Initialize queue structures.
   */
  void initQueue();

  /**
   * @brief Initialize gate graph from file.
   * @param gategraphfile Path to gate graph file
   */
  void initGateGraph(const char *gategraphfile);

  /**
   * @brief Initialize gates from file.
   * @param gatefile Path to gate file
   */
  void initGates(const char *gatefile);

  /**
   * @brief Initialize index from file.
   * @param indexfile Path to index file
   */
  virtual void initIndex(const char *indexfile);

  /**
   * @brief Output index to file.
   * @param out_index_file Path to output index file
   */
  void outIndex(const char *out_index_file);

  /**
   * @brief Set the method name.
   * @param _method_name Name of the method
   */
  void setMethodName(string _method_name);

  /**
   * @brief Get the method name.
   * @return Method name string
   */
  string getMethodName() const;

  /**
   * @brief Get the file stem.
   * @return File stem string
   */
  string getFilestem() const;

  /**
   * @brief Get the gate size.
   * @return Number of gates
   */
  int getGateSize() const;

  /**
   * @brief Get the gate edge size.
   * @return Number of gate edges
   */
  int getGateEdgeSize();

  /**
   * @brief Get the total index size.
   * @return Total index size in bytes
   */
  virtual long getIndexSize() const;

  /**
   * @brief Get detailed index size breakdown.
   * @return Vector of index sizes
   */
  virtual vector<long> indexSize() const;

  /**
   * @brief Set the radius parameter.
   * @param r Radius value
   */
  void setRadius(int r);

  /**
   * @brief Generate gate graph filename based on radius and pre-selection
   * ratio.
   * @param filestem File stem string
   * @return Vector of generated filenames
   */
  vector<string> makeggfilename(const char *filestem);

  // methods for query
  /**
   * @brief Check reachability between source and target vertices.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool reach(int src, int trg) override;

  /**
   * @brief Check reachability without using materialization.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  virtual bool reachWithoutMat(int src, int trg);

  /**
   * @brief Test reachability with additional validation.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  virtual bool test_reach(int src, int trg);

  /**
   * @brief Test non-reachability.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if not reachable, false otherwise
   */
  virtual bool test_nomreach(int src, int trg);

  // options for improvement
  /**
   * @brief Compute local gates for optimization.
   * @param ispartial Flag for partial computation
   */
  virtual void computeLocalGates(bool ispartial);

  /**
   * @brief Compute multiple labels for quick rejection.
   * @param num_labels Number of labels to compute
   */
  virtual void computeMultiLabelsQuickRej(int num_labels);

  /**
   * @brief Display index information.
   * @param out Output stream
   */
  void displayIndex(ostream &out);

  /**
   * @brief Display gates information.
   * @param out Output stream
   */
  void displayGates(ostream &out);

  /**
   * @brief Display labels for a specific node.
   * @param vid Vertex ID
   * @param out Output stream
   */
  virtual void displayLabelsByNode(int vid, ostream &out);

  /**
   * @brief Display all labels.
   * @param out Output stream
   */
  virtual void displayLabels(ostream &out);

  /**
   * @brief Display GRAIL labels.
   * @param out Output stream
   */
  virtual void displayGrailLabels(ostream &out);

  /**
   * @brief Display local gates.
   * @param out Output stream
   */
  virtual void displayLocalGates(ostream &out);

  /**
   * @brief Display local gates for a specific node.
   * @param vid Vertex ID
   * @param out Output stream
   */
  virtual void displayLocalGatesByNode(int vid, ostream &out);

  // methods for inheritance
  /**
   * @brief Select partial nodes for local gates computation.
   * @param nodes Vector to store selected nodes
   * @param num Number of nodes to select
   */
  virtual void selectPartialNodes(vector<int> &nodes, int num);

  /**
   * @brief Perform multiple labeling for optimization.
   * @param num_labels Number of labels to generate
   */
  virtual void mutipleLabeling(int num_labels);

  /**
   * @brief Compute topological ordering of vertices.
   */
  void computeTopoOrder();

  /**
   * @brief Initialize materialization structures.
   */
  virtual void initMaterialization();

  /**
   * @brief Perform materialization for specified number of vertices.
   * @param num Number of vertices to materialize
   */
  virtual void materialization(int num);

  /**
   * @brief Select vertices for materialization.
   * @param num Number of vertices to select
   */
  virtual void selectMaterialized(int num);

  /**
   * @brief Materialize incoming neighbors for a vertex.
   * @param vid Vertex ID
   */
  virtual void materializeInNeighbors(int vid);

  /**
   * @brief Precompute gates for a vertex.
   * @param vid Vertex ID
   * @param out Flag for outgoing gates
   */
  virtual void precomputeGates(int vid, bool out);

  /**
   * @brief Display general information.
   * @param out Output stream
   */
  virtual void displayInfor(ostream &out);

  /**
   * @brief Display information for a specific vertex.
   * @param vid Vertex ID
   * @param out Output stream
   */
  virtual void displayInfor(int vid, ostream &out);

public:
  const char *method() const override { return "PathTree"; }

  void reset() override {}
};


} // namespace lotus::cfl::cs_index::flare

#endif

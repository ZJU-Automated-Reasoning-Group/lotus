#pragma once

#include "CFL/CSIndex/FLARE/Grail/Index.h"
#include "CFL/CSIndex/SCS/PolicyAutomaton.h"
#include "CFL/CSIndex/SCS/Graph.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace lotus::cfl::cs_index::scs {

enum class ProductConstruction { Explicit, Lazy };

struct BatchQuery {
  std::string name;
  std::vector<int> sources;
  std::vector<int> sinks;
};

struct IndexOptions {
  ProductConstruction product_construction = ProductConstruction::Lazy;
  bool retain_witnesses = false;
  int grail_dimensions = 2;
};

struct QueryStats {
  size_t queries = 0;
  size_t positive_queries = 0;
  uint64_t total_time_ns = 0;
  uint64_t max_time_ns = 0;

  double averageMicroseconds() const {
    return queries == 0 ? 0.0
                        : static_cast<double>(total_time_ns) / queries / 1000.0;
  }
};

struct IndexStats {
  size_t base_vertices = 0;
  size_t base_edges = 0;
  size_t policy_states = 0;
  size_t accepting_policy_states = 0;
  size_t indexed_sources = 0;
  size_t indexed_sinks = 0;
  size_t indexed_batches = 0;

  size_t explicit_product_states = 0;
  size_t materialized_product_states = 0;
  size_t product_vertices = 0;
  size_t product_edges = 0;
  size_t normalized_product_edges = 0;
  size_t summary_edges = 0;
  size_t flare_vertices = 0;
  size_t flare_edges = 0;
  size_t indexing_vertices = 0;
  size_t indexing_edges = 0;
  size_t dag_vertices = 0;
  size_t dag_edges = 0;

  uint64_t validation_time_ns = 0;
  uint64_t product_construction_time_ns = 0;
  uint64_t product_copy_time_ns = 0;
  uint64_t summary_construction_time_ns = 0;
  uint64_t flare_transformation_time_ns = 0;
  uint64_t endpoint_augmentation_time_ns = 0;
  uint64_t scc_condensation_time_ns = 0;
  uint64_t reachability_index_time_ns = 0;
  uint64_t total_construction_time_ns = 0;
  uint64_t process_peak_rss_before_bytes = 0;
  uint64_t process_peak_rss_after_bytes = 0;

  QueryStats point_queries;
  QueryStats batch_queries;
  QueryStats witness_queries;

  double materializedProductFraction() const {
    return explicit_product_states == 0
               ? 0.0
               : static_cast<double>(materialized_product_states) /
                     explicit_product_states;
  }

  double totalConstructionMilliseconds() const {
    return static_cast<double>(total_construction_time_ns) / 1000000.0;
  }

  static void writeCsvHeader(std::ostream &output);
  void writeCsvRow(std::ostream &output) const;
};

struct Witness {
  std::vector<int> base_vertices;
  std::vector<int> base_edges;
  std::vector<int> structural_labels;
  std::vector<int> event_labels;
  bool context_valid = false;
  bool policy_accepting = false;
};

/**
 * Sanitizer-aware context-sensitive reachability index. Security events are
 * compiled into a product graph before the existing FLARE transformation.
 */
class Index {
public:
  Index(const Graph &graph, const PolicyAutomaton &policy,
           std::vector<int> sources, std::vector<int> sinks,
           IndexOptions options = {},
           std::vector<BatchQuery> batches = {});
  ~Index();

  Index(const Index &) = delete;
  Index &operator=(const Index &) = delete;

  bool reachable(int source, int sink);
  bool reachableBatch(const std::string &batch_name);
  std::optional<Witness> witness(int source, int sink) const;

  int startVertex(int source) const;
  int acceptVertex(int sink) const;
  int productVertex(int base_vertex, int policy_state) const;

  flare::Graph &productGraph();
  flare::Graph &indexingGraph();
  const IndexStats &stats() const;

private:
  struct ProductVertexInfo {
    int base_vertex = -1;
    int policy_state = -1;
    bool intermediate = false;
  };

  std::pair<int, bool> getOrCreateProductVertex(int base_vertex,
                                                int policy_state);
  void addProductTransition(int source_product, int target_product,
                            const Edge &base_edge);
  void addGraphEdge(flare::Graph &graph, int source, int target, int label);
  void buildExplicitProduct();
  void buildLazyProduct();
  void buildIndex(const std::vector<BatchQuery> &batches);
  void validateCatalog() const;

  std::vector<int> findOrdinaryPath(int source, int target) const;
  std::optional<std::vector<int>>
  expandProductPath(const std::vector<int> &ordinary_path) const;
  static void appendPath(std::vector<int> &path,
                         const std::vector<int> &suffix);
  static void recordQuery(QueryStats &query_stats, uint64_t elapsed_ns,
                          bool positive);
  bool contextValid(const std::vector<int> &structural_labels) const;
  bool policyAccepting(const std::vector<int> &event_labels) const;

  Graph base_graph_;
  PolicyAutomaton policy_;
  std::vector<int> sources_;
  std::vector<int> sinks_;
  IndexOptions options_;

  std::unique_ptr<flare::Graph> product_graph_;
  std::unique_ptr<flare::Graph> indexing_graph_;
  std::unique_ptr<flare::Graph> dag_graph_;
  std::unique_ptr<flare::grail::Index> grail_;

  std::vector<ProductVertexInfo> product_vertices_;
  std::map<std::pair<int, int>, int> product_vertex_map_;
  std::map<std::pair<int, int>, int> product_edge_origins_;
  std::map<int, int> start_vertices_;
  std::map<int, int> accept_vertices_;
  std::map<std::string, std::pair<int, int>> batch_vertices_;
  std::vector<int> scc_map_;

  int flare_vertex_count_ = 0;
  mutable IndexStats stats_;
};

} // namespace lotus::cfl::cs_index::scs

#include "CFL/CSIndex/SCS/Index.h"

#include "CFL/CSIndex/FLARE/GraphAlgorithms.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <ostream>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>

namespace lotus::cfl::cs_index::scs {
#endif

namespace {

using Clock = std::chrono::steady_clock;

uint64_t elapsedNanoseconds(Clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
}

uint64_t processPeakResidentSetBytes() {
#if defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return 0;
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
#else
  return 0;
#endif
}

} // namespace

void IndexStats::writeCsvHeader(std::ostream &output) {
  output
      << "base_vertices,base_edges,policy_states,accepting_policy_states,"
         "indexed_sources,indexed_sinks,indexed_batches,"
         "explicit_product_states,materialized_product_states,"
         "materialized_product_fraction,product_vertices,product_edges,"
         "normalized_product_edges,summary_edges,flare_vertices,flare_edges,"
         "indexing_vertices,indexing_edges,dag_vertices,dag_edges,"
         "validation_time_ns,product_construction_time_ns,product_copy_time_ns,"
         "summary_construction_time_ns,flare_transformation_time_ns,"
         "endpoint_augmentation_time_ns,scc_condensation_time_ns,"
         "reachability_index_time_ns,total_construction_time_ns,"
         "process_peak_rss_before_bytes,process_peak_rss_after_bytes,"
         "point_queries,positive_point_queries,point_total_time_ns,"
         "point_max_time_ns,point_average_us,batch_queries,"
         "positive_batch_queries,batch_total_time_ns,batch_max_time_ns,"
         "batch_average_us,witness_queries,positive_witness_queries,"
         "witness_total_time_ns,witness_max_time_ns,witness_average_us";
}

void IndexStats::writeCsvRow(std::ostream &output) const {
  output << base_vertices << ',' << base_edges << ',' << policy_states << ','
         << accepting_policy_states << ',' << indexed_sources << ','
         << indexed_sinks << ',' << indexed_batches << ','
         << explicit_product_states << ',' << materialized_product_states << ','
         << materializedProductFraction() << ',' << product_vertices << ','
         << product_edges << ',' << normalized_product_edges << ','
         << summary_edges << ',' << flare_vertices << ',' << flare_edges << ','
         << indexing_vertices << ',' << indexing_edges << ',' << dag_vertices
         << ',' << dag_edges << ',' << validation_time_ns << ','
         << product_construction_time_ns << ',' << product_copy_time_ns << ','
         << summary_construction_time_ns << ',' << flare_transformation_time_ns
         << ',' << endpoint_augmentation_time_ns << ','
         << scc_condensation_time_ns << ',' << reachability_index_time_ns << ','
         << total_construction_time_ns << ',' << process_peak_rss_before_bytes
         << ',' << process_peak_rss_after_bytes << ',' << point_queries.queries
         << ',' << point_queries.positive_queries << ','
         << point_queries.total_time_ns << ',' << point_queries.max_time_ns
         << ',' << point_queries.averageMicroseconds() << ','
         << batch_queries.queries << ',' << batch_queries.positive_queries
         << ',' << batch_queries.total_time_ns << ','
         << batch_queries.max_time_ns << ','
         << batch_queries.averageMicroseconds() << ','
         << witness_queries.queries << ',' << witness_queries.positive_queries
         << ',' << witness_queries.total_time_ns << ','
         << witness_queries.max_time_ns << ','
         << witness_queries.averageMicroseconds();
}

Index::Index(const Graph &graph, const PolicyAutomaton &policy,
                   std::vector<int> sources, std::vector<int> sinks,
                   IndexOptions options, std::vector<BatchQuery> batches)
    : base_graph_(graph), policy_(policy), sources_(std::move(sources)),
      sinks_(std::move(sinks)), options_(options),
      product_graph_(std::make_unique<flare::Graph>()) {
  const auto construction_start = Clock::now();
  stats_.process_peak_rss_before_bytes = processPeakResidentSetBytes();
  const auto validation_start = Clock::now();
  validateCatalog();

  std::string policy_error;
  if (!policy_.validate(base_graph_.observedEvents(), &policy_error))
    throw std::invalid_argument(policy_error);
  if (options_.grail_dimensions <= 0)
    throw std::invalid_argument("GRAIL dimensions must be positive");
  stats_.validation_time_ns = elapsedNanoseconds(validation_start);

  stats_.base_vertices = base_graph_.num_vertices();
  stats_.base_edges = base_graph_.num_edges();
  stats_.policy_states = policy_.stateCount();
  stats_.accepting_policy_states = policy_.acceptingStates().size();
  stats_.indexed_sources = sources_.size();
  stats_.indexed_sinks = sinks_.size();
  stats_.indexed_batches = batches.size();

  stats_.explicit_product_states =
      static_cast<size_t>(base_graph_.num_vertices()) * policy_.stateCount();
  const auto product_start = Clock::now();
  if (options_.product_construction == ProductConstruction::Explicit)
    buildExplicitProduct();
  else
    buildLazyProduct();
  stats_.product_construction_time_ns = elapsedNanoseconds(product_start);
  buildIndex(batches);
  stats_.total_construction_time_ns = elapsedNanoseconds(construction_start);
  stats_.process_peak_rss_after_bytes = processPeakResidentSetBytes();
}

Index::~Index() = default;

void Index::validateCatalog() const {
  if (sources_.empty())
    throw std::invalid_argument("An SCS index requires at least one source");
  if (sinks_.empty())
    throw std::invalid_argument("An SCS index requires at least one sink");

  for (int source : sources_) {
    if (!base_graph_.hasVertex(source))
      throw std::invalid_argument("Source vertex is out of range");
  }
  for (int sink : sinks_) {
    if (!base_graph_.hasVertex(sink))
      throw std::invalid_argument("Sink vertex is out of range");
  }
}

std::pair<int, bool> Index::getOrCreateProductVertex(int base_vertex,
                                                        int policy_state) {
  const auto key = std::make_pair(base_vertex, policy_state);
  const auto existing = product_vertex_map_.find(key);
  if (existing != product_vertex_map_.end())
    return {existing->second, false};

  const int product_vertex = product_graph_->num_vertices();
  product_graph_->addVertex(product_vertex);
  product_graph_->at(product_vertex).func_id =
      base_graph_.vertex(base_vertex).func_id;
  product_vertex_map_[key] = product_vertex;
  product_vertices_.push_back({base_vertex, policy_state, false});
  return {product_vertex, true};
}

void Index::addGraphEdge(flare::Graph &graph, int source, int target,
                         int label) {
  if (label == 0)
    graph.addEdge(source, target);
  else
    graph.addEdge(source, target, label);
}

void Index::addProductTransition(int source_product, int target_product,
                                    const Edge &base_edge) {
  const auto direct_key = std::make_pair(source_product, target_product);
  if (!product_graph_->hasEdge(source_product, target_product)) {
    addGraphEdge(*product_graph_, source_product, target_product,
                 base_edge.structural_label);
    product_edge_origins_[direct_key] = base_edge.id;
    return;
  }

  if (product_graph_->label(source_product, target_product) ==
      base_edge.structural_label) {
    // These product edges are semantically identical for SCS-LCR. Retain one
    // representative base edge for witness projection.
    return;
  }

  // Graph labels are keyed by endpoint pair. Preserve a colliding structural
  // role with the isolated degree-two normalization permitted by the model.
  const int intermediate = product_graph_->num_vertices();
  product_graph_->addVertex(intermediate);
  product_graph_->at(intermediate).func_id =
      base_graph_.vertex(base_edge.source).func_id;
  product_vertices_.push_back({-1, -1, true});

  product_graph_->addEdge(source_product, intermediate);
  addGraphEdge(*product_graph_, intermediate, target_product,
               base_edge.structural_label);
  product_edge_origins_[{source_product, intermediate}] = -1;
  product_edge_origins_[{intermediate, target_product}] = base_edge.id;
  ++stats_.normalized_product_edges;
}

void Index::buildExplicitProduct() {
  for (int vertex = 0; vertex < base_graph_.num_vertices(); ++vertex) {
    for (int state = 0; state < policy_.stateCount(); ++state)
      getOrCreateProductVertex(vertex, state);
  }

  for (const Edge &edge : base_graph_.edges()) {
    for (int state = 0; state < policy_.stateCount(); ++state) {
      const int source_product = productVertex(edge.source, state);
      for (int successor : policy_.successors(state, edge.event_label)) {
        const int target_product = productVertex(edge.target, successor);
        addProductTransition(source_product, target_product, edge);
      }
    }
  }
}

void Index::buildLazyProduct() {
  std::deque<int> worklist;
  for (int source : sources_) {
    auto product = getOrCreateProductVertex(source, policy_.initialState());
    if (product.second)
      worklist.push_back(product.first);
  }

  while (!worklist.empty()) {
    const int source_product = worklist.front();
    worklist.pop_front();
    const ProductVertexInfo source_info = product_vertices_[source_product];
    assert(!source_info.intermediate);

    for (int edge_id : base_graph_.out_edges(source_info.base_vertex)) {
      const Edge &edge = base_graph_.edge(edge_id);
      for (int successor :
           policy_.successors(source_info.policy_state, edge.event_label)) {
        auto target = getOrCreateProductVertex(edge.target, successor);
        addProductTransition(source_product, target.first, edge);
        if (target.second)
          worklist.push_back(target.first);
      }
    }
  }
}

void Index::buildIndex(const std::vector<BatchQuery> &batches) {
  stats_.materialized_product_states = product_vertex_map_.size();
  stats_.product_vertices = product_graph_->num_vertices();
  stats_.product_edges = product_graph_->num_edges();

  auto phase_start = Clock::now();
  indexing_graph_ = std::make_unique<flare::Graph>(*product_graph_);
  stats_.product_copy_time_ns = elapsedNanoseconds(phase_start);

  phase_start = Clock::now();
  indexing_graph_->build_summary_edges(options_.retain_witnesses);
  stats_.summary_construction_time_ns = elapsedNanoseconds(phase_start);
  stats_.summary_edges = indexing_graph_->summary_edge_size();

  flare_vertex_count_ = indexing_graph_->num_vertices();
  phase_start = Clock::now();
  indexing_graph_->to_indexing_graph();
  stats_.flare_transformation_time_ns = elapsedNanoseconds(phase_start);
  stats_.flare_vertices = indexing_graph_->num_vertices();
  stats_.flare_edges = indexing_graph_->num_edges();

  phase_start = Clock::now();
  for (int source : sources_)
    start_vertices_[source] = productVertex(source, policy_.initialState());

  for (int sink : sinks_) {
    const int accept = indexing_graph_->num_vertices();
    indexing_graph_->addVertex(accept);
    accept_vertices_[sink] = accept;

    for (int accepting_state : policy_.acceptingStates()) {
      const int product = productVertex(sink, accepting_state);
      if (product >= 0)
        indexing_graph_->addEdge(product + flare_vertex_count_, accept);
    }
  }

  for (const BatchQuery &batch : batches) {
    if (batch.name.empty())
      throw std::invalid_argument("Batch query names cannot be empty");
    if (batch_vertices_.count(batch.name))
      throw std::invalid_argument("Duplicate batch query name");

    const int virtual_source = indexing_graph_->num_vertices();
    indexing_graph_->addVertex(virtual_source);
    const int virtual_target = indexing_graph_->num_vertices();
    indexing_graph_->addVertex(virtual_target);

    for (int source : batch.sources)
      indexing_graph_->addEdge(virtual_source, startVertex(source));
    for (int sink : batch.sinks)
      indexing_graph_->addEdge(acceptVertex(sink), virtual_target);
    batch_vertices_[batch.name] = {virtual_source, virtual_target};
  }
  stats_.endpoint_augmentation_time_ns = elapsedNanoseconds(phase_start);

  stats_.indexing_vertices = indexing_graph_->num_vertices();
  stats_.indexing_edges = indexing_graph_->num_edges();

  phase_start = Clock::now();
  dag_graph_ = std::make_unique<flare::Graph>(*indexing_graph_);
  scc_map_.resize(indexing_graph_->num_vertices());
  std::vector<int> reverse_topological_order;
  flare::GraphAlgorithms::mergeSCC(*dag_graph_, scc_map_.data(),
                                   reverse_topological_order);
  flare::GraphAlgorithms::topo_leveler(*dag_graph_);
  stats_.scc_condensation_time_ns = elapsedNanoseconds(phase_start);
  stats_.dag_vertices = dag_graph_->num_vertices();
  stats_.dag_edges = dag_graph_->num_edges();

  phase_start = Clock::now();
  grail_ = std::make_unique<flare::grail::Index>(
      *dag_graph_, options_.grail_dimensions, 1, false, 100);
  stats_.reachability_index_time_ns = elapsedNanoseconds(phase_start);
}

bool Index::reachable(int source, int sink) {
  const int start = scc_map_.at(startVertex(source));
  const int accept = scc_map_.at(acceptVertex(sink));
  const auto query_start = Clock::now();
  const bool result = grail_->reach(start, accept);
  recordQuery(stats_.point_queries, elapsedNanoseconds(query_start), result);
  return result;
}

bool Index::reachableBatch(const std::string &batch_name) {
  const auto batch_it = batch_vertices_.find(batch_name);
  if (batch_it == batch_vertices_.end())
    throw std::out_of_range("Unknown SCS batch query");
  const int start = scc_map_.at(batch_it->second.first);
  const int accept = scc_map_.at(batch_it->second.second);
  const auto query_start = Clock::now();
  const bool result = grail_->reach(start, accept);
  recordQuery(stats_.batch_queries, elapsedNanoseconds(query_start), result);
  return result;
}

int Index::startVertex(int source) const {
  const auto source_it = start_vertices_.find(source);
  if (source_it == start_vertices_.end())
    throw std::out_of_range("Source is not in the indexed catalog");
  return source_it->second;
}

int Index::acceptVertex(int sink) const {
  const auto sink_it = accept_vertices_.find(sink);
  if (sink_it == accept_vertices_.end())
    throw std::out_of_range("Sink is not in the indexed catalog");
  return sink_it->second;
}

int Index::productVertex(int base_vertex, int policy_state) const {
  const auto product_it = product_vertex_map_.find({base_vertex, policy_state});
  if (product_it == product_vertex_map_.end())
    return -1;
  return product_it->second;
}

flare::Graph &Index::productGraph() { return *product_graph_; }

flare::Graph &Index::indexingGraph() { return *indexing_graph_; }

const IndexStats &Index::stats() const { return stats_; }

std::vector<int> Index::findOrdinaryPath(int source, int target) const {
  std::vector<int> parent(indexing_graph_->num_vertices(), -1);
  std::deque<int> worklist;
  parent[source] = source;
  worklist.push_back(source);

  while (!worklist.empty() && parent[target] == -1) {
    const int vertex = worklist.front();
    worklist.pop_front();
    for (int successor : indexing_graph_->out_edges(vertex)) {
      if (parent[successor] != -1)
        continue;
      parent[successor] = vertex;
      worklist.push_back(successor);
    }
  }

  if (parent[target] == -1)
    return {};

  std::vector<int> path;
  for (int vertex = target;; vertex = parent[vertex]) {
    path.push_back(vertex);
    if (vertex == source)
      break;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

void Index::appendPath(std::vector<int> &path,
                          const std::vector<int> &suffix) {
  if (suffix.empty())
    return;
  size_t begin = 0;
  if (!path.empty() && path.back() == suffix.front())
    begin = 1;
  path.insert(path.end(), suffix.begin() + begin, suffix.end());
}

void Index::recordQuery(QueryStats &query_stats, uint64_t elapsed_ns,
                           bool positive) {
  ++query_stats.queries;
  if (positive)
    ++query_stats.positive_queries;
  query_stats.total_time_ns += elapsed_ns;
  query_stats.max_time_ns = std::max(query_stats.max_time_ns, elapsed_ns);
}

std::optional<std::vector<int>>
Index::expandProductPath(const std::vector<int> &ordinary_path) const {
  if (ordinary_path.empty())
    return std::nullopt;

  std::vector<int> product_path;
  product_path.push_back(ordinary_path.front());

  for (size_t index = 1; index < ordinary_path.size(); ++index) {
    const int source = ordinary_path[index - 1];
    const int target = ordinary_path[index];

    if (source >= 2 * flare_vertex_count_ ||
        target >= 2 * flare_vertex_count_) {
      continue;
    }
    if (source < flare_vertex_count_ &&
        target == source + flare_vertex_count_) {
      continue;
    }

    const bool first_layer =
        source < flare_vertex_count_ && target < flare_vertex_count_;
    const bool second_layer =
        source >= flare_vertex_count_ && target >= flare_vertex_count_;
    if (!first_layer && !second_layer)
      return std::nullopt;

    const int product_source = source % flare_vertex_count_;
    const int product_target = target % flare_vertex_count_;
    if (product_graph_->hasEdge(product_source, product_target)) {
      appendPath(product_path, {product_source, product_target});
      continue;
    }

    const std::vector<int> *summary =
        indexing_graph_->summary_witness(product_source, product_target);
    if (!summary)
      return std::nullopt;
    appendPath(product_path, *summary);
  }
  return product_path;
}

bool Index::contextValid(const std::vector<int> &structural_labels) const {
  std::vector<int> calls;
  for (int label : structural_labels) {
    if (label > 0) {
      calls.push_back(label);
    } else if (label < 0 && !calls.empty()) {
      if (calls.back() + label != 0)
        return false;
      calls.pop_back();
    }
  }
  return true;
}

bool Index::policyAccepting(const std::vector<int> &event_labels) const {
  std::set<int> states = {policy_.initialState()};
  for (int event : event_labels) {
    std::set<int> successors;
    for (int state : states) {
      const std::vector<int> next = policy_.successors(state, event);
      successors.insert(next.begin(), next.end());
    }
    states = std::move(successors);
    if (states.empty())
      return false;
  }

  for (int state : states) {
    if (policy_.isAccepting(state))
      return true;
  }
  return false;
}

std::optional<Witness> Index::witness(int source, int sink) const {
  const auto query_start = Clock::now();
  const auto result = [&]() -> std::optional<Witness> {
    if (!options_.retain_witnesses)
      return std::nullopt;

    const std::vector<int> ordinary_path =
        findOrdinaryPath(startVertex(source), acceptVertex(sink));
    const auto product_path = expandProductPath(ordinary_path);
    if (!product_path)
      return std::nullopt;

    Witness witness_result;
    witness_result.base_vertices.push_back(source);
    int current_vertex = source;
    for (size_t index = 1; index < product_path->size(); ++index) {
      const auto origin_it = product_edge_origins_.find(
          {product_path->at(index - 1), product_path->at(index)});
      if (origin_it == product_edge_origins_.end())
        return std::nullopt;
      if (origin_it->second < 0)
        continue;

      const Edge &edge = base_graph_.edge(origin_it->second);
      if (edge.source != current_vertex)
        return std::nullopt;
      current_vertex = edge.target;
      witness_result.base_edges.push_back(edge.id);
      witness_result.base_vertices.push_back(edge.target);
      witness_result.structural_labels.push_back(edge.structural_label);
      witness_result.event_labels.push_back(edge.event_label);
    }

    if (current_vertex != sink)
      return std::nullopt;
    witness_result.context_valid =
        contextValid(witness_result.structural_labels);
    witness_result.policy_accepting =
        policyAccepting(witness_result.event_labels);
    if (!witness_result.context_valid || !witness_result.policy_accepting)
      return std::nullopt;
    return witness_result;
  }();

  recordQuery(stats_.witness_queries, elapsedNanoseconds(query_start),
              result.has_value());
  return result;
}

} // namespace lotus::cfl::cs_index::scs

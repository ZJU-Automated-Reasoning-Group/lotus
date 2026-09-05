#pragma once

#include "CFL/Classical/Solvers/Engines/POCR/SpecializedEngines.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {

enum class AliasConstraintEdgeKind {
  Addr,
  Copy,
  Store,
  Load,
  NormalGep,
  VariantGep,
  MemoryTransfer,
};

struct AliasConstraintEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  AliasConstraintEdgeKind kind = AliasConstraintEdgeKind::Copy;
  std::optional<std::uint32_t> attribute;
  /// For VariantGep, offsets are congruent to attribute modulo this value.
  /// A missing modulus means an arbitrary byte offset.
  std::optional<std::uint32_t> modulus;
};

class AliasConstraintGraph {
public:
  std::size_t addNode(const std::string &name);
  void addEdge(std::size_t source, std::size_t target,
               AliasConstraintEdgeKind kind,
               std::optional<std::uint32_t> attribute = std::nullopt,
               std::optional<std::uint32_t> modulus = std::nullopt);

  const std::vector<std::string> &nodeNames() const { return node_names_; }
  const std::vector<AliasConstraintEdge> &edges() const { return edges_; }

private:
  std::vector<std::string> node_names_;
  std::vector<AliasConstraintEdge> edges_;
};

enum class AliasEncodingMode {
  PAG,
  PEG,
};

LabeledGraph encodePagGraph(const AliasConstraintGraph &graph);
LabeledGraph encodePegGraph(const AliasConstraintGraph &graph);
Grammar buildPagGrammar(const AliasConstraintGraph &graph);
Grammar buildPegGrammar(const AliasConstraintGraph &graph);

class AliasClient {
public:
  ~AliasClient();
  AliasClient(AliasClient &&other) noexcept;
  AliasClient &operator=(AliasClient &&other) noexcept;
  AliasClient(const AliasClient &) = delete;
  AliasClient &operator=(const AliasClient &) = delete;

  static AliasClient
  fromConstraintGraph(const AliasConstraintGraph &graph,
                      AliasEncodingMode mode = AliasEncodingMode::PAG);

  ReachabilityStats solve(SolverBackend backend = SolverBackend::SparseSet);
  ReachabilityStats solveSpecialized(engines::SpecializedPocrBackend backend,
                                     bool simplify_focr_cycles = false);
  /// Alternate solving with a client-supplied discovery policy. The callback
  /// may add nodes and constraints and returns true when it changed the input.
  ReachabilityStats solveToFixedPoint(
      SolverBackend backend,
      const std::function<bool(AliasClient &)> &discover_constraints,
      std::size_t max_rounds = 64);
  ReachabilityStats solveToFixedPoint(
      engines::SpecializedPocrBackend backend,
      const std::function<bool(AliasClient &)> &discover_constraints,
      std::size_t max_rounds = 64, bool simplify_focr_cycles = false);
  std::size_t addNode(const std::string &name);
  /// Add a constraint after construction. If solving has started, the same
  /// solver session is resumed on the next solve() call.
  bool addConstraint(std::size_t source, std::size_t target,
                     AliasConstraintEdgeKind kind,
                     std::optional<std::uint32_t> attribute = std::nullopt,
                     std::optional<std::uint32_t> modulus = std::nullopt);
  /// Copy pointer-bearing memory cells in [source, source + size) to target.
  /// A missing size conservatively represents an unknown-length transfer.
  bool addMemoryTransfer(std::size_t source, std::size_t target,
                         std::optional<std::uint32_t> size);
  /// Extend the attributed GEP grammar once for a batch of constraints.
  bool registerGepAttributes(const std::set<std::uint32_t> &attributes);
  bool mayAlias(std::size_t lhs, std::size_t rhs) const;
  /// Query only the solved CFL/POCR value relation, without triggering the
  /// optional object-valued points-to refinement.
  bool mayValueAlias(std::size_t lhs, std::size_t rhs) const;
  /// Project solved value-alias facts to explicit address-taken objects. This
  /// lightweight relation is used for incremental indirect-call discovery.
  std::vector<std::size_t> addressTakenObjects(std::size_t ptr) const;
  std::unordered_map<std::size_t, std::vector<std::size_t>>
  addressTakenObjects(const std::vector<std::size_t> &pointers) const;
  std::unordered_map<std::size_t, std::vector<std::size_t>>
  matchingAddressTakenObjects(
      const std::vector<std::size_t> &pointers,
      const std::vector<std::pair<std::size_t, std::size_t>>
          &object_pointer_candidates) const;
  /// Return abstract object nodes, never pointer/value nodes. Non-zero and
  /// unknown-offset fields are represented by synthetic object nodes.
  std::vector<std::size_t> pointsTo(std::size_t ptr) const;
  /// Map a synthetic field object back to its allocation object.
  std::optional<std::size_t> baseObject(std::size_t pointee) const;

  const LabeledGraph &graph() const;
  const Grammar &grammar() const;

private:
  AliasClient(LabeledGraph graph, Grammar grammar,
              AliasConstraintGraph constraints, AliasEncodingMode mode);

  struct AbstractLocation {
    std::size_t base = 0;
    std::uint64_t offset = 0;
    /// No modulus is an exact offset; otherwise this is a congruence class.
    std::optional<std::uint64_t> modulus;

    bool operator<(const AbstractLocation &other) const {
      return std::tie(base, offset, modulus) <
             std::tie(other.base, other.offset, other.modulus);
    }
  };

  std::size_t addInternalNode(const std::string &name);
  bool addEncodedEdge(std::size_t source, std::size_t target,
                      const std::string &forward, const std::string &reverse);
  const std::vector<std::size_t> &ensurePegDereferences(std::size_t pointer);
  void initializePegDereferences();
  void initializeGepAttributes();
  void rebuildGrammar();
  void rebuildPointsTo() const;
  void indexAddressTakenObjects(const std::vector<std::size_t> &pointers) const;
  LabeledGraph buildSpecializedAliasGraph() const;
  bool pointsToOverlap(std::size_t lhs, std::size_t rhs) const;
  static bool locationsOverlap(const AbstractLocation &lhs,
                               const AbstractLocation &rhs);
  void invalidateSpecializedEngines();

  struct State;
  std::unique_ptr<State> state_;
  AliasConstraintGraph constraints_;
  AliasEncodingMode mode_ = AliasEncodingMode::PAG;
  std::unordered_map<std::size_t, std::vector<std::size_t>> peg_dereferences_;
  std::size_t next_synthetic_dereference_ = 0;
  std::set<std::uint32_t> gep_attributes_;
  bool grammar_dirty_ = false;
  mutable std::vector<std::set<AbstractLocation>> points_to_;
  mutable std::map<AbstractLocation, std::size_t> location_nodes_;
  mutable std::unordered_map<std::size_t, std::size_t> pointee_bases_;
  mutable bool points_to_valid_ = false;
  mutable std::unordered_map<std::size_t, std::vector<std::size_t>>
      address_objects_;
  mutable std::unordered_set<std::size_t> address_object_sources_;
  mutable bool address_objects_valid_ = false;
  std::unique_ptr<SolverSession> session_;
  std::optional<SolverBackend> backend_;
  std::unique_ptr<engines::PocrAliasEngine> pocr_engine_;
  std::unique_ptr<engines::FocrAliasEngine> focr_engine_;
  std::unique_ptr<LabeledGraph> specialized_graph_;
  std::optional<engines::SpecializedPocrBackend> specialized_backend_;
  bool specialized_focr_cycles_ = false;
};

} // namespace lotus::cfl::classical

#pragma once

#include "CFL/Classical/Clients/ValueFlow/SVFGPreparation.h"
#include "CFL/Classical/Solvers/Engines/POCR/SpecializedEngines.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lotus::analysis {
class SVFG;
} // namespace lotus::analysis

namespace lotus::cfl::classical {

LabeledGraph encodeSVFG(const lotus::analysis::SVFG &svfg);
Grammar buildVfgGrammar(const lotus::analysis::SVFG &svfg);

/// Context-sensitive may-reach value-flow facade. Direct, indirect-memory,
/// and may-happen-in-parallel input edges remain distinguishable in the
/// encoded graph, while the derived A relation is their sound union.
class ValueFlowClient {
public:
  ~ValueFlowClient();
  ValueFlowClient(ValueFlowClient &&other) noexcept;
  ValueFlowClient &operator=(ValueFlowClient &&other) noexcept;
  ValueFlowClient(const ValueFlowClient &) = delete;
  ValueFlowClient &operator=(const ValueFlowClient &) = delete;

  static ValueFlowClient fromSVFG(const lotus::analysis::SVFG &svfg);
  static ValueFlowClient
  fromPreparedSVFG(lotus::analysis::SVFG &svfg,
                   const SVFGPreparationOptions &options = {});

  ReachabilityStats solve(SolverBackend backend = SolverBackend::SparseSet);
  ReachabilityStats solveSpecialized(engines::SpecializedPocrBackend backend,
                                     bool simplify_focr_cycles = false);
  bool hasFlow(std::uint32_t source_node, std::uint32_t target_node) const;
  std::vector<std::uint32_t> reachableFrom(std::uint32_t source_node) const;

  const LabeledGraph &graph() const;
  const Grammar &grammar() const;

private:
  ValueFlowClient(
      LabeledGraph graph, Grammar grammar,
      std::unordered_map<std::uint32_t, std::size_t> node_to_vertex);

  struct State;
  std::unique_ptr<State> state_;
  std::unordered_map<std::uint32_t, std::size_t> node_to_vertex_;
  std::vector<std::optional<std::uint32_t>> vertex_to_node_;
  std::unique_ptr<SolverSession> session_;
  std::optional<SolverBackend> backend_;
  std::unique_ptr<engines::PocrValueFlowEngine> pocr_engine_;
  std::unique_ptr<engines::FocrValueFlowEngine> focr_engine_;
  std::optional<engines::SpecializedPocrBackend> specialized_backend_;
  bool specialized_focr_cycles_ = false;
};

} // namespace lotus::cfl::classical

#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace lotus::cfl::classical::engines::stg {

/// One element of a DNF regular-expression sequence. `symbols` represents a
/// union; `kleene_star` changes that union into its reflexive transitive
/// closure.
struct RegularAtom {
  std::vector<SymbolId> symbols;
  bool kleene_star = false;
};

using RegularSequence = std::vector<RegularAtom>;

struct RegularProduction {
  SymbolId lhs = 0;
  std::vector<RegularSequence> alternatives;
};

struct DelimiterPair {
  SymbolId open = 0;
  SymbolId close = 0;
};

/// Sum -> open_i (body | Sum)* close_i.
struct DyckCfp {
  SymbolId summary = 0;
  std::vector<DelimiterPair> delimiters;
  std::vector<SymbolId> body_symbols;
};

/// X -> open reverse_forward* Y backward* close (Algorithm 1).
struct AliasCfp {
  SymbolId summary = 0;
  SymbolId open = 0;
  SymbolId close = 0;
  SymbolId reverse_forward = 0;
  SymbolId center = 0;
  SymbolId backward = 0;
};

struct StagedSpecification {
  std::vector<RegularProduction> phase_l_regular;
  std::vector<DyckCfp> dyck_patterns;
  std::vector<AliasCfp> alias_patterns;
  std::vector<RegularProduction> phase_r;
};

/// CFP decomposition from Section 3.2.1 of the Stg paper.
StagedSpecification
decomposeStandardDyck(SymbolId start, SymbolId summary,
                      std::vector<SymbolId> neutral_symbols,
                      std::vector<DelimiterPair> delimiters);

/// CFP decomposition from Section 3.2.2 of the Stg paper.
StagedSpecification
decomposeExtendedDyck(SymbolId start, SymbolId summary,
                      std::vector<SymbolId> neutral_symbols,
                      std::vector<DelimiterPair> delimiters);

/// CFP decomposition boundary for Section 3.3. The regular definitions are
/// the rewritten A/Y/B equations in P_L; phase_r contains the rewritten
/// regular client result expressions.
StagedSpecification
decomposeAliasCfp(AliasCfp pattern,
                  std::vector<RegularProduction> phase_l_regular,
                  std::vector<RegularProduction> phase_r = {});

struct StagedStatistics {
  std::size_t phase_l_rounds = 0;
  std::size_t phase_l_regular_edges = 0;
  std::size_t dyck_path_edges = 0;
  std::size_t alias_forward_path_edges = 0;
  std::size_t alias_backward_path_edges = 0;
  std::size_t summary_edges = 0;
  std::size_t phase_r_productions = 0;
  std::size_t phase_r_edges = 0;
  std::size_t ordered_scc_propagations = 0;
};

/// ISSTA'24 staged CFL-reachability solver.
class StagedSolver {
public:
  StagedSolver(const Grammar &grammar, Relation &relation,
               StagedSpecification specification, std::size_t node_count = 0);
  ~StagedSolver();
  StagedSolver(StagedSolver &&) noexcept;
  StagedSolver &operator=(StagedSolver &&) noexcept;
  StagedSolver(const StagedSolver &) = delete;
  StagedSolver &operator=(const StagedSolver &) = delete;

  void ensureNodeCount(std::size_t node_count);
  bool addEdge(SymbolId symbol, NodeId source, NodeId target);
  StagedStatistics solve();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines::stg

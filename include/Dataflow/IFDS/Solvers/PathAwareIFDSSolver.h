/*
 * Path-Aware IFDS Solver
 *
 * This solver wraps the path-aware IDE solver to provide path tracking
 * for IFDS problems.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Solvers/PathAwareIDESolver.h"

namespace ifds {

template <typename Problem> class PathAwareIFDSSolver {
public:
  using Fact = typename Problem::FactType;
  using FactSet = typename Problem::FactSet;
  using Node = typename ExplodedSupergraph<Fact>::Node;
  using NodeHash = typename ExplodedSupergraph<Fact>::NodeHash;
  using ESGEdge = typename ExplicitExplodedSupergraph<Fact>::ESGEdge;

  // Wrap the IFDS problem as an IDE problem with binary domain
  struct BinaryValue {
    bool present;
    BinaryValue() : present(false) {}
    explicit BinaryValue(bool p) : present(p) {}

    static BinaryValue top() { return BinaryValue(true); }
    static BinaryValue bottom() { return BinaryValue(false); }

    bool operator==(const BinaryValue &other) const {
      return present == other.present;
    }
  };

  class IDEWrapper : public IDEProblem<Fact, BinaryValue> {
  public:
    IDEWrapper(Problem &ifds_problem) : m_ifds_problem(ifds_problem) {}

    Fact zero_fact() const override { return m_ifds_problem.zero_fact(); }
    bool auto_add_zero() const override {
      return m_ifds_problem.auto_add_zero();
    }
    bool is_zero_fact(const Fact &fact) const override {
      return m_ifds_problem.is_zero_fact(fact);
    }
    void set_alias_analysis(lotus::AliasAnalysisWrapper *aa) override {
      IDEProblem<Fact, BinaryValue>::set_alias_analysis(aa);
      m_ifds_problem.set_alias_analysis(aa);
    }
    bool has_alias_analysis_configured() const {
      return m_ifds_problem.has_alias_analysis_configured();
    }
    bool is_source(const llvm::Instruction *inst) const override {
      return m_ifds_problem.is_source(inst);
    }
    bool is_sink(const llvm::Instruction *inst) const override {
      return m_ifds_problem.is_sink(inst);
    }

    FactSet normal_flow(const llvm::Instruction *stmt,
                        const llvm::Instruction *succ,
                        const Fact &fact) override {
      return m_ifds_problem.normal_flow(stmt, succ, fact);
    }

    FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                      const Fact &fact) override {
      return m_ifds_problem.call_flow(call, callee, fact);
    }

    FactSet return_flow(const llvm::CallBase *call,
                        const llvm::Instruction *exit_inst,
                        const llvm::Instruction *return_site,
                        const llvm::Function *callee, const Fact &exit_fact,
                        const Fact &call_fact) override {
      return m_ifds_problem.return_flow(call, exit_inst, return_site, callee,
                                        exit_fact, call_fact);
    }

    FactSet call_to_return_flow(const llvm::CallBase *call,
                                const llvm::Instruction *return_site,
                                llvm::ArrayRef<const llvm::Function *> callees,
                                const Fact &fact) override {
      return m_ifds_problem.call_to_return_flow(call, return_site, callees,
                                                fact);
    }

    FactSet initial_facts(const llvm::Function *main) override {
      return m_ifds_problem.initial_facts(main);
    }

    typename IFDSProblem<Fact>::InitialSeeds
    initial_seeds(const llvm::Module &module) override {
      return m_ifds_problem.initial_seeds(module);
    }
    typename IDEProblem<Fact, BinaryValue>::IDEInitialSeeds
    initial_ide_seeds(const llvm::Module &module) override {
      return this->lift_ifds_initial_seeds(module, bottom_value());
    }

    // IDE interface - identity edge functions
    typename IDEProblem<Fact, BinaryValue>::EdgeFunction
    normal_edge_function(const llvm::Instruction *, const llvm::Instruction *,
                         const Fact &, const Fact &) override {
      return this->identity();
    }

    typename IDEProblem<Fact, BinaryValue>::EdgeFunction
    call_edge_function(const llvm::CallBase *, const llvm::Function *,
                       const Fact &, const Fact &) override {
      return this->identity();
    }

    typename IDEProblem<Fact, BinaryValue>::EdgeFunction
    return_edge_function(const llvm::CallBase *, const llvm::Function *,
                         const llvm::Instruction *, const llvm::Instruction *,
                         const Fact &, const Fact &) override {
      return this->identity();
    }

    typename IDEProblem<Fact, BinaryValue>::EdgeFunction
    call_to_return_edge_function(const llvm::CallBase *,
                                 const llvm::Instruction *,
                                 llvm::ArrayRef<const llvm::Function *>,
                                 const Fact &, const Fact &) override {
      return this->identity();
    }

    BinaryValue top_value() const override { return BinaryValue::top(); }
    BinaryValue bottom_value() const override { return BinaryValue::bottom(); }

    BinaryValue join(const BinaryValue &v1,
                     const BinaryValue &v2) const override {
      return BinaryValue(v1.present || v2.present);
    }

  private:
    Problem &m_ifds_problem;
  };

  PathAwareIFDSSolver(Problem &problem)
      : m_wrapper(problem), m_solver(m_wrapper) {}

  void solve(const llvm::Module &module) { m_solver.solve(module); }

  // Query interface (IFDS style)
  FactSet get_facts_at_entry(const llvm::Instruction *inst) const {
    FactSet facts;
    std::vector<PathEdge<Fact>> edges;
    m_solver.get_path_edges(edges);
    for (const auto &edge : edges) {
      if (edge.target_node == inst) {
        facts.insert(edge.target_fact);
      }
    }
    return facts;
  }

  // Access the explicit ESG
  const ExplicitExplodedSupergraph<Fact> &get_esg() const {
    return m_solver.get_esg();
  }

  // Path queries
  std::vector<std::vector<ESGEdge>>
  find_paths(const llvm::Instruction *start_inst, const Fact &start_fact,
             const llvm::Instruction *end_inst, const Fact &end_fact,
             size_t max_paths = 10, size_t max_depth = 100) const {
    return m_solver.find_paths(start_inst, start_fact, end_inst, end_fact,
                               max_paths, max_depth);
  }

  // Export ESG
  void export_esg_to_dot(const std::string &filename) const {
    m_solver.export_esg_to_dot(filename);
  }

  void export_esg_to_dot(llvm::raw_ostream &os) const {
    m_solver.export_esg_to_dot(os);
  }

  // Statistics
  size_t get_esg_node_count() const { return m_solver.get_esg_node_count(); }
  size_t get_esg_edge_count() const { return m_solver.get_esg_edge_count(); }

  // Solver configuration
  void set_solver_config(IFDSIDESolverConfig config) {
    m_solver.set_solver_config(std::move(config));
  }

  IFDSIDESolverConfig &get_solver_config() {
    return m_solver.get_solver_config();
  }

private:
  IDEWrapper m_wrapper;
  PathAwareIDESolver<IDEWrapper> m_solver;
};

} // namespace ifds

#ifndef DATAFLOW_APA_CORE_PROBLEM_H_
#define DATAFLOW_APA_CORE_PROBLEM_H_

#include <cstddef>
#include <utility>
#include <vector>

namespace elimination {

// Generic intraprocedural elimination problem contract.
//
// Path-expression evaluation interprets:
//   - Union  via meet(Lhs, Rhs)
//   - Concat via applyTransfer composition order
//   - Star   via iterative meet until equal_to stabilizes
//
// Therefore:
//   - meetIdentity() must be the neutral element for meet
//   - initialFact() is the seed fact at entry() before any transfer is applied
//   - edgeTransfer(Src, Dst) + applyTransfer() must agree on flow direction
template <typename AnalysisDomainTy> class IntraEliminationProblem {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using fact_t = typename AnalysisDomainTy::fact_t;
  using transfer_t = typename AnalysisDomainTy::transfer_t;

  virtual ~IntraEliminationProblem() = default;

  virtual std::vector<n_t> nodes() const = 0;
  virtual n_t entry() const = 0;
  virtual std::vector<n_t> succs(n_t Node) const = 0;

  virtual transfer_t edgeTransfer(n_t Src, n_t Dst) const = 0;
  virtual fact_t applyTransfer(const transfer_t &T, const fact_t &In) const = 0;

  virtual fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const = 0;
  virtual bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const = 0;

  virtual fact_t meetIdentity() const = 0;
  virtual fact_t initialFact() const = 0;

  virtual std::size_t maxStarIterations() const { return 100000; }
};

template <typename AnalysisDomainTy>
class IntraReducibleEliminationProblem
    : public IntraEliminationProblem<AnalysisDomainTy> {
public:
  using Base = IntraEliminationProblem<AnalysisDomainTy>;
  using n_t = typename Base::n_t;

  struct Edge final {
    n_t Src{};
    n_t Dst{};
  };

  virtual std::vector<Edge> edges() const = 0;
  // Topological order of the non-back-edge subgraph, with entry first.
  // ADT engines use this to build interval ranges and classify crossings.
  virtual std::vector<n_t> topologicalOrder() const = 0;
  // Immediate dominator in the problem's flow direction.
  virtual n_t idom(n_t Node) const = 0;
  // Dominance relation in the problem's flow direction.
  virtual bool dominates(n_t A, n_t B) const = 0;

  virtual bool isBackEdge(n_t Src, n_t Dst) const {
    return dominates(Dst, Src);
  }
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_PROBLEM_H_

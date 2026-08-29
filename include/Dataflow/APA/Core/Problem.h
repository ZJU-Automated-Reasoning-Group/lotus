#ifndef DATAFLOW_APA_CORE_PROBLEM_H_
#define DATAFLOW_APA_CORE_PROBLEM_H_

#include "Dataflow/APA/Core/AbstractDomain.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace elimination {

// Generic intraprocedural elimination problem contract.
//
// Path-expression evaluation interprets:
//   - Union  via join(Lhs, Rhs)
//   - Concat via applyTransfer composition order
//   - Star   via iterative join until equal stabilizes
//
// Therefore:
//   - bottom() must be the neutral element for join
//   - initialFact() is the seed fact at entry() before any transfer is applied
//   - edgeTransfer(Src, Dst) + applyTransfer() must agree on flow direction
template <typename AnalysisTypesT> class IntraEliminationProblem {
public:
  using n_t = typename AnalysisTypesT::n_t;
  using fact_t = typename AnalysisTypesT::fact_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;
  using abstract_domain_t = typename AnalysisTypesT::abstract_domain_t;

  static_assert(IsAPAAbstractDomain<abstract_domain_t>::value,
                "APA domain must define value_type, bottom, join, and equal");
  static_assert(std::is_same<typename abstract_domain_t::value_type,
                             fact_t>::value,
                "APA analysis types and domain must use the same fact type");

  explicit IntraEliminationProblem(
      abstract_domain_t Domain = abstract_domain_t{})
      : AbstractDomainState(std::move(Domain)) {}

  virtual ~IntraEliminationProblem() = default;

  virtual std::vector<n_t> nodes() const = 0;
  virtual n_t entry() const = 0;
  virtual std::vector<n_t> succs(n_t Node) const = 0;

  virtual transfer_t edgeTransfer(n_t Src, n_t Dst) const = 0;
  virtual fact_t applyTransfer(const transfer_t &T, const fact_t &In) const = 0;

  virtual fact_t join(const fact_t &Lhs, const fact_t &Rhs) const {
    return AbstractDomainState.join(Lhs, Rhs);
  }
  virtual bool equal(const fact_t &Lhs, const fact_t &Rhs) const {
    return AbstractDomainState.equal(Lhs, Rhs);
  }

  virtual fact_t bottom() const { return AbstractDomainState.bottom(); }
  virtual fact_t initialFact() const = 0;

  virtual std::size_t maxStarIterations() const { return 100000; }

  abstract_domain_t &getAbstractDomain() { return AbstractDomainState; }
  const abstract_domain_t &getAbstractDomain() const {
    return AbstractDomainState;
  }

private:
  abstract_domain_t AbstractDomainState;
};

template <typename AnalysisTypesT>
class IntraReducibleEliminationProblem
    : public IntraEliminationProblem<AnalysisTypesT> {
public:
  using Base = IntraEliminationProblem<AnalysisTypesT>;
  using Base::Base;
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

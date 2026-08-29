#ifndef DATAFLOW_APA_CORE_INTERPROBLEM_H_
#define DATAFLOW_APA_CORE_INTERPROBLEM_H_

#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/APA/Core/AbstractDomain.h"

#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {

template <typename AnalysisTypesT> class InterEliminationProblem {
public:
  using n_t = typename AnalysisTypesT::n_t;
  using fact_t = typename AnalysisTypesT::fact_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;
  using f_t = typename AnalysisTypesT::f_t;
  using i_t = typename AnalysisTypesT::i_t;
  using abstract_domain_t = typename AnalysisTypesT::abstract_domain_t;

  static_assert(IsAPAAbstractDomain<abstract_domain_t>::value,
                "APA domain must define value_type, bottom, join, and equal");
  static_assert(std::is_same<typename abstract_domain_t::value_type,
                             fact_t>::value,
                "APA analysis types and domain must use the same fact type");

  explicit InterEliminationProblem(std::vector<f_t> EntryPoints = {},
                                   const i_t *ICF = nullptr,
                                   abstract_domain_t Domain = abstract_domain_t{})
      : EntryPoints(std::move(EntryPoints)), ICF(ICF),
        AbstractDomainState(std::move(Domain)) {}

  virtual ~InterEliminationProblem() = default;

  virtual fact_t normalFlow(n_t Inst, const fact_t &In) = 0;
  virtual fact_t join(const fact_t &Lhs, const fact_t &Rhs) const {
    return AbstractDomainState.join(Lhs, Rhs);
  }
  virtual bool equal(const fact_t &Lhs, const fact_t &Rhs) const {
    return AbstractDomainState.equal(Lhs, Rhs);
  }

  virtual transfer_t edgeTransfer(n_t Src, n_t Dst) const {
    if constexpr (std::is_same_v<transfer_t, n_t>) {
      return direction() == ::dataflow::controlflow::FlowDirection::Backward
                 ? Dst
                 : Src;
    } else {
      (void)Src;
      (void)Dst;
      return transfer_t{};
    }
  }

  virtual fact_t applyTransfer(const transfer_t &T, const fact_t &In) const {
    if constexpr (std::is_same_v<transfer_t, n_t>) {
      return const_cast<InterEliminationProblem *>(this)->normalFlow(T, In);
    } else {
      (void)T;
      return In;
    }
  }

  virtual n_t transferNode(const transfer_t &T) const {
    if constexpr (std::is_same_v<transfer_t, n_t>) {
      return T;
    } else {
      (void)T;
      return n_t{};
    }
  }

  virtual n_t transferSuccessor(const transfer_t &T) const {
    (void)T;
    return n_t{};
  }

  virtual fact_t bottom() const { return AbstractDomainState.bottom(); }

  virtual std::unordered_map<n_t, fact_t> initialSeeds() = 0;

  virtual ::dataflow::controlflow::FlowDirection direction() const {
    return ::dataflow::controlflow::FlowDirection::Forward;
  }

  virtual fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) = 0;
  virtual fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t RetSite,
                            const fact_t &In) = 0;
  virtual fact_t returnFlowWithCallerFact(n_t CallSite, f_t Callee,
                                          n_t ExitStmt, n_t RetSite,
                                          const fact_t &CalleeExit,
                                          const fact_t &CallerFact) {
    (void)CallerFact;
    return returnFlow(CallSite, Callee, ExitStmt, RetSite, CalleeExit);
  }
  virtual fact_t callToRetFlow(n_t CallSite, n_t RetSite,
                               const std::vector<f_t> &Callees,
                               const fact_t &In) = 0;

  virtual std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const = 0;

  const std::vector<f_t> &getEntryPoints() const { return EntryPoints; }
  const i_t *getICFG() const { return ICF; }
  abstract_domain_t &getAbstractDomain() { return AbstractDomainState; }
  const abstract_domain_t &getAbstractDomain() const {
    return AbstractDomainState;
  }

private:
  std::vector<f_t> EntryPoints;
  const i_t *ICF = nullptr;
  abstract_domain_t AbstractDomainState;
};

} // namespace elimination

#endif // DATAFLOW_APA_CORE_INTERPROBLEM_H_

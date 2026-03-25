#ifndef NPA_PREDICATE_RELATION_DOMAIN_H
#define NPA_PREDICATE_RELATION_DOMAIN_H

#include "Dataflow/NPA/Core/NPACommon.h"

#include <array>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace npa {

class PredicateRelation {
public:
  struct Impl;

  PredicateRelation();
  explicit PredicateRelation(std::shared_ptr<Impl> impl_in);
  PredicateRelation(const PredicateRelation &);
  PredicateRelation(PredicateRelation &&) noexcept;
  PredicateRelation &operator=(const PredicateRelation &);
  PredicateRelation &operator=(PredicateRelation &&) noexcept;
  ~PredicateRelation();

  std::shared_ptr<Impl> impl;
};

class PredicateRelationDomain {
public:
  using value_type = PredicateRelation;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static void configure(unsigned predicate_count,
                        unsigned local_predicate_count = 0);
  static void configure(unsigned predicate_count,
                        const std::vector<unsigned> &local_predicates);
  static bool isConfigured();
  static unsigned getPredicateCount();
  static unsigned getLocalPredicateCount();
  static unsigned getGlobalPredicateCount();
  static bool isLocalPredicate(unsigned predicate);

  static value_type zero();
  static value_type one();
  static bool equal(const value_type &a, const value_type &b);
  static value_type combine(const value_type &a, const value_type &b);
  static value_type ndetCombine(const value_type &a, const value_type &b);
  static value_type condCombine(bool /*phi*/, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &outer, const value_type &inner);
  static value_type extend_lin(const value_type &outer,
                               const value_type &inner);
  static value_type subtract(const value_type &a, const value_type &b);

  static value_type assume(unsigned predicate, bool truthy);
  static value_type assignConst(unsigned predicate, bool value);
  static value_type transpose(const value_type &relation);
  static value_type project(const value_type &relation);
  static value_type merge(const value_type &lhs, const value_type &rhs);
  static value_type fromTransitions(
      const std::vector<std::pair<std::uint64_t, std::uint64_t>> &transitions);

  static std::vector<std::pair<std::uint64_t, std::uint64_t>>
  materialize(const value_type &relation);
};

class PredicateTensorRelation {
public:
  struct Impl;

  PredicateTensorRelation();
  explicit PredicateTensorRelation(std::shared_ptr<Impl> impl_in);
  PredicateTensorRelation(const PredicateTensorRelation &);
  PredicateTensorRelation(PredicateTensorRelation &&) noexcept;
  PredicateTensorRelation &operator=(const PredicateTensorRelation &);
  PredicateTensorRelation &operator=(PredicateTensorRelation &&) noexcept;
  ~PredicateTensorRelation();

  std::shared_ptr<Impl> impl;
};

class PredicateTensorDomain {
public:
  using value_type = PredicateTensorRelation;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static value_type zero();
  static value_type one();
  static bool equal(const value_type &a, const value_type &b);
  static value_type combine(const value_type &a, const value_type &b);
  static value_type ndetCombine(const value_type &a, const value_type &b);
  static value_type condCombine(bool /*phi*/, const value_type &t,
                                const value_type &e);
  static value_type extend(const value_type &outer, const value_type &inner);
  static value_type extend_lin(const value_type &outer,
                               const value_type &inner);
  static value_type subtract(const value_type &a, const value_type &b);

  static value_type couple(const PredicateRelation &lhs,
                           const PredicateRelation &rhs);
  static PredicateRelation readout(const value_type &relation);
  static value_type projectT(const value_type &relation);
  static value_type merge(const value_type &lhs, const value_type &rhs);
  static bool validatePaperLaws();
  static value_type fromTransitions(
      const std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                                   std::uint64_t>> &transitions);

  static std::vector<
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
  materialize(const value_type &relation);
};

} // namespace npa

#include "Dataflow/NPA/Core/TensorSemiring.h"

namespace npa {

template <> struct TensorSemiringTraits<PredicateRelationDomain> {
  using tensor_domain = PredicateTensorDomain;

  static bool available() { return true; }
  static bool paper_admissible() { return true; }
  static bool paper_projection_equations() { return true; }
  static bool validate_paper_laws() {
    return PredicateTensorDomain::validatePaperLaws();
  }

  static tensor_domain::value_type
  right_constant(const PredicateRelationDomain::value_type &value) {
    return tensor_domain::couple(PredicateRelationDomain::one(), value);
  }

  static tensor_domain::value_type
  left_constant(const PredicateRelationDomain::value_type &value) {
    return tensor_domain::couple(value, PredicateRelationDomain::one());
  }

  static tensor_domain::value_type
  constant(const PredicateRelationDomain::value_type &value) {
    return right_constant(value);
  }

  static tensor_domain::value_type
  couple(const PredicateRelationDomain::value_type &lhs,
         const PredicateRelationDomain::value_type &rhs) {
    return tensor_domain::couple(lhs, rhs);
  }

  static PredicateRelationDomain::value_type
  readout(const tensor_domain::value_type &value) {
    return tensor_domain::readout(value);
  }
};

} // namespace npa

#endif // NPA_PREDICATE_RELATION_DOMAIN_H

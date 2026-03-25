#ifndef NPA_COMMON_H
#define NPA_COMMON_H

/**
 * \file
 * \brief NPA common types and domain concept (semiring).
 *
 * Newtonian Program Analysis (NPA) solves systems of equations over
 * ω-continuous semirings. The framework expects a \e domain (semiring) D with:
 * - combine (⊕), extend (⊗), extend_lin (linearized equations), zero (⊥), one
 * (1)
 * - subtract is required only for non-idempotent domains
 *
 * References:
 * - Esparza et al., "Newtonian Program Analysis" (JACM): Newton sequence
 *   ν^(i+1) = ν^(i) + Δ^(i) where Δ^(i) is the least solution of the linearized
 *   system Df|ν^(i)(X) + δ^(i) = X.
 * - Reps et al., "Newtonian Program Analysis via Tensor Product" (TOPLAS 2016):
 *   Solving the LCFL linear sub-problems via tensor-product regularization.
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace npa {

using Symbol = std::string;

/// Strategy for solving the \e linearized equation system on each Newton round.
/// The linear system has the form Df|ν(X) + δ = X (LCFL equation system).
enum class LinearStrategy {
  /// Vector fixpoint: update all variables each round (chaotic iteration).
  Naive,
  /// Global SCC scheduling with dependency-driven worklist solving inside each
  /// SCC. Independent SCC layers may execute in parallel.
  SCC,
  /// Newton linearization solved SCC-by-SCC with a deterministic local choice
  /// among direct evaluation, SCC worklist, and tensor solving.
  AdaptiveScc,
  /// Tensor-product (TOPLAS 2016): lift LCFL system to paired semiring,
  /// solve as left-linear (regular) system, then project back. Only used when
  /// the linear system has LCFL structure (Concat/Star).
  TensorProduct
};

enum class DomainContractMode {
  Off,
  BasicChecks,
};

enum class IndirectCallResolutionMode {
  ClosedWorldTypeCompatible,
  DeclaredOnlyFallback,
  CustomResolverRequired,
};

template <class T> inline void hash_combine(std::size_t &h, const T &v) {
  h ^= std::hash<T>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
}

struct Stat {
  double time{};
  int iters{};
  bool converged = true;
  bool hit_limit = false;
  bool hit_outer_limit = false;
  bool hit_linear_limit = false;
  bool hit_fixpoint_limit = false;
  int equation_count = 0;
  int requested_max_iters = -1;
  int effective_max_iters = -1;
  LinearStrategy linear_strategy = LinearStrategy::SCC;
  bool used_approx_equal = false;
  bool used_auto_n_cap = false;
  bool retried_without_auto_n_cap = false;
  bool adaptive_scc_used = false;
  int adaptive_scc_direct_count = 0;
  int adaptive_scc_worklist_count = 0;
  int adaptive_scc_tensor_count = 0;
  int adaptive_scc_tensor_fallback_count = 0;
  bool domain_contract_checks_run = false;
  bool domain_contract_checks_failed = false;
};

struct AnalysisStatus {
  Stat summary_solve;
  long propagation_steps = 0;
  bool propagation_converged = true;
  bool propagation_hit_limit = false;
  bool configuration_error = false;
  bool unsupported_specs = false;
  bool approximated = false;
  bool used_summary_overflow = false;
  bool used_fact_widening = false;
  bool used_bounded_inner_solve = false;
  bool overall_converged = true;
  bool overall_hit_limit = false;
  IndirectCallResolutionMode call_resolution_mode =
      IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  long indirect_calls_seen = 0;
  long unresolved_indirect_calls = 0;
  long fallback_call_edges = 0;
  bool requires_external_callee_resolver = false;
  bool open_world_unsound_mode = true;
};

/**********************************************************************
 * Domain concept (ω-continuous semiring)
 *
 * Required: zero, one, combine (⊕), extend (⊗), extend_lin, ndetCombine,
 * condCombine, equal. See Esparza et al. for the semiring axioms and
 * ω-continuity; NPA uses the least fixed point μf of f. subtract() is
 * required only for non-idempotent domains.
 *********************************************************************/
template <class D> struct DomainHasBase {
  template <class T>
  static auto test(int)
      -> decltype(T::zero(), T::one(), T::combine(T::zero(), T::zero()),
                  T::extend(T::zero(), T::zero()),
                  T::extend_lin(T::zero(), T::zero()),
                  T::ndetCombine(T::zero(), T::zero()),
                  T::condCombine(typename T::test_type{}, T::zero(), T::zero()),
                  T::equal(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasSubtract {
  template <class T>
  static auto test(int)
      -> decltype(T::subtract(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasChooseDelta {
  template <class T>
  static auto test(int)
      -> decltype(T::choose_delta(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasApproxEqual {
  template <class T>
  static auto test(int)
      -> decltype(T::approx_equal(T::zero(), T::zero()), std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasCommutativeExtend {
  template <class T>
  static auto test(int) -> decltype(T::commutative_extend, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasProject {
  template <class T>
  static auto test(int) ->
      typename std::enable_if<std::is_same<decltype(T::project(T::zero())),
                                           typename T::value_type>::value,
                              std::true_type>::type;
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasProjectT {
  template <class T>
  static auto test(int) ->
      typename std::enable_if<std::is_same<decltype(T::projectT(T::zero())),
                                           typename T::value_type>::value,
                              std::true_type>::type;
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasProjectNewtonSafe {
  template <class T>
  static auto test(int) -> decltype(T::project_newton_safe, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasMaxFixpointIters {
  template <class T>
  static auto test(int) -> decltype(T::max_fixpoint_iters, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> struct DomainHasMaxLinearSteps {
  template <class T>
  static auto test(int) -> decltype(T::max_linear_steps, std::true_type{});
  template <class> static std::false_type test(...);

public:
  static constexpr bool value =
      std::is_same<decltype(test<D>(0)), std::true_type>::value;
};

template <class D> using DomVal = typename D::value_type;
template <class D> using DomTest = typename D::test_type;

#define NPA_REQUIRE_DOMAIN(D)                                                  \
  static_assert(DomainHasBase<D>::value,                                       \
                "Invalid DOMAIN: missing required methods");                   \
  static_assert(                                                               \
      D::idempotent || DomainHasSubtract<D>::value ||                          \
          DomainHasChooseDelta<D>::value,                                      \
      "Non-idempotent DOMAIN must implement subtract() or choose_delta()")

struct Dirty {
  mutable bool dirty_ = true;
  void mark(bool d = true) const { dirty_ = d; }
};

template <class V> struct Optional {
  bool has{false};
  V val{};
  Optional() = default;
  Optional(const Optional &) = default;
  Optional &operator=(const Optional &) = default;
  Optional &operator=(const V &v_in) {
    val = v_in;
    has = true;
    return *this;
  }
  void reset() { has = false; }
  bool has_value() const { return has; }
  V &operator*() { return val; }
  const V &operator*() const { return val; }
};

namespace detail {
template <class D>
inline bool domain_equal_impl(const DomVal<D> &a, const DomVal<D> &b,
                              std::true_type) {
  return D::approx_equal(a, b);
}
template <class D>
inline bool domain_equal_impl(const DomVal<D> &a, const DomVal<D> &b,
                              std::false_type) {
  return D::equal(a, b);
}
} // namespace detail

/// Default equality used across solvers.
///
/// Paper-faithful domains should normally rely on exact semiring equality.
/// Domains may provide approx_equal() as a pragmatic extension for numeric
/// semirings where exact equality is too strong for convergence; once used,
/// stability/convergence checks become approximate rather than theorem-exact.
template <class D>
inline bool domain_equal(const DomVal<D> &a, const DomVal<D> &b) {
  return detail::domain_equal_impl<D>(
      a, b, std::integral_constant<bool, DomainHasApproxEqual<D>::value>{});
}

template <class D>
inline bool domain_exact_equal(const DomVal<D> &a, const DomVal<D> &b) {
  return D::equal(a, b);
}

/// Natural order for idempotent semirings: a ⊑ b  iff  a ⊕ b = b.
template <class D>
inline bool domain_leq_idempotent(const DomVal<D> &a, const DomVal<D> &b) {
  static_assert(D::idempotent,
                "domain_leq_idempotent requires an idempotent domain");
  return domain_equal<D>(D::combine(a, b), b);
}

namespace detail {
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &v, std::true_type,
                                     std::false_type) {
  return D::project(v);
}
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &v, std::false_type,
                                     std::true_type) {
  return D::projectT(v);
}
template <class D>
inline DomVal<D> domain_project_impl(const DomVal<D> &,
                                     std::false_type /* has_project */,
                                     std::false_type /* has_project_t */) {
  assert(false && "Domain must implement project() or projectT() to evaluate "
                  "projection expressions");
  return D::zero();
}
} // namespace detail

template <class D> inline DomVal<D> domain_project(const DomVal<D> &v) {
  return detail::domain_project_impl<D>(
      v, std::integral_constant<bool, DomainHasProject<D>::value>{},
      std::integral_constant<bool, DomainHasProjectT<D>::value>{});
}

namespace detail {
template <class D> inline bool domain_commutative_extend_impl(std::true_type) {
  return D::commutative_extend;
}
template <class D> inline bool domain_commutative_extend_impl(std::false_type) {
  return false;
}
template <class D> inline bool domain_project_newton_safe_impl(std::true_type) {
  return D::project_newton_safe;
}
template <class D>
inline bool domain_project_newton_safe_impl(std::false_type) {
  return false;
}
} // namespace detail

/// Returns true if D declares commutative_extend, otherwise false.
template <class D> inline bool domain_commutative_extend() {
  return detail::domain_commutative_extend_impl<D>(
      std::integral_constant<bool, DomainHasCommutativeExtend<D>::value>{});
}

template <class D> inline bool domain_project_newton_safe() {
  return detail::domain_project_newton_safe_impl<D>(
      std::integral_constant<bool, DomainHasProjectNewtonSafe<D>::value>{});
}

namespace detail {
template <class D> inline int domain_max_fixpoint_iters_impl(std::true_type) {
  return D::max_fixpoint_iters;
}
template <class D> inline int domain_max_fixpoint_iters_impl(std::false_type) {
  return -1;
}
template <class D> inline long domain_max_linear_steps_impl(std::true_type) {
  return static_cast<long>(D::max_linear_steps);
}
template <class D> inline long domain_max_linear_steps_impl(std::false_type) {
  return -1;
}
} // namespace detail

template <class D> inline int domain_max_fixpoint_iters() {
  return detail::domain_max_fixpoint_iters_impl<D>(
      std::integral_constant<bool, DomainHasMaxFixpointIters<D>::value>{});
}

template <class D> inline long domain_max_linear_steps() {
  return detail::domain_max_linear_steps_impl<D>(
      std::integral_constant<bool, DomainHasMaxLinearSteps<D>::value>{});
}

struct NoopDomainRunState {};

struct NoopDomainRunStateScope {
  explicit NoopDomainRunStateScope(const NoopDomainRunState &) {}
};

template <class D> struct DomainExecutionStateTraits {
  using state_type = NoopDomainRunState;
  using scope_type = NoopDomainRunStateScope;

  static state_type capture() { return {}; }
};

template <class Tag> class DomainWidthContext {
public:
  struct state_type {
    bool active = false;
    unsigned bit_width = 1;
  };

  class scope_type {
  public:
    scope_type() = default;

    explicit scope_type(unsigned bit_width)
        : scope_type(state_type{true, bit_width}) {}

    explicit scope_type(const state_type &state) { reset(state); }

    scope_type(const scope_type &) = delete;
    scope_type &operator=(const scope_type &) = delete;

    scope_type(scope_type &&other) noexcept
        : previous_width_(other.previous_width_),
          previous_active_(other.previous_active_),
          installed_(other.installed_) {
      other.installed_ = false;
    }

    scope_type &operator=(scope_type &&other) noexcept {
      if (this == &other)
        return *this;
      restore();
      previous_width_ = other.previous_width_;
      previous_active_ = other.previous_active_;
      installed_ = other.installed_;
      other.installed_ = false;
      return *this;
    }

    ~scope_type() { restore(); }

    void reset(unsigned bit_width) { reset(state_type{true, bit_width}); }

    void reset(const state_type &state) {
      restore();
      previous_width_ = current_bit_width_slot();
      previous_active_ = has_current_bit_width_slot();
      installed_ = true;
      if (state.active) {
        current_bit_width_slot() = state.bit_width;
        has_current_bit_width_slot() = true;
      } else {
        current_bit_width_slot() = 1;
        has_current_bit_width_slot() = false;
      }
    }

  private:
    void restore() {
      if (!installed_)
        return;
      current_bit_width_slot() = previous_width_;
      has_current_bit_width_slot() = previous_active_;
      installed_ = false;
    }

    unsigned previous_width_;
    bool previous_active_;
    bool installed_ = false;
  };

  static state_type capture() {
    return state_type{has_current_bit_width_slot(), current_bit_width_slot()};
  }

  static unsigned require(const char *message) {
    assert(has_current_bit_width_slot() && message);
    return current_bit_width_slot();
  }

private:
  static unsigned &current_bit_width_slot() {
    static thread_local unsigned width = 1;
    return width;
  }

  static bool &has_current_bit_width_slot() {
    static thread_local bool active = false;
    return active;
  }
};

struct ApproximationSourceFlags {
  bool hit_outer_limit = false;
  bool hit_linear_limit = false;
  bool hit_fixpoint_limit = false;
};

struct AdaptiveSccSolveStats {
  bool used = false;
  int direct_count = 0;
  int worklist_count = 0;
  int tensor_count = 0;
  int tensor_fallback_count = 0;
};

class ApproximationSourceCollector {
public:
  void reset() {
    hit_outer_limit_.store(false, std::memory_order_relaxed);
    hit_linear_limit_.store(false, std::memory_order_relaxed);
    hit_fixpoint_limit_.store(false, std::memory_order_relaxed);
  }

  void note_outer_limit_hit() {
    hit_outer_limit_.store(true, std::memory_order_relaxed);
  }

  void note_linear_limit_hit() {
    hit_linear_limit_.store(true, std::memory_order_relaxed);
  }

  void note_fixpoint_limit_hit() {
    hit_fixpoint_limit_.store(true, std::memory_order_relaxed);
  }

  ApproximationSourceFlags snapshot() const {
    ApproximationSourceFlags flags;
    flags.hit_outer_limit =
        hit_outer_limit_.load(std::memory_order_relaxed);
    flags.hit_linear_limit =
        hit_linear_limit_.load(std::memory_order_relaxed);
    flags.hit_fixpoint_limit =
        hit_fixpoint_limit_.load(std::memory_order_relaxed);
    return flags;
  }

private:
  std::atomic<bool> hit_outer_limit_{false};
  std::atomic<bool> hit_linear_limit_{false};
  std::atomic<bool> hit_fixpoint_limit_{false};
};

class AdaptiveSccSolveCollector {
public:
  void reset() {
    used_.store(false, std::memory_order_relaxed);
    direct_count_.store(0, std::memory_order_relaxed);
    worklist_count_.store(0, std::memory_order_relaxed);
    tensor_count_.store(0, std::memory_order_relaxed);
    tensor_fallback_count_.store(0, std::memory_order_relaxed);
  }

  void note_used() { used_.store(true, std::memory_order_relaxed); }

  void note_direct(int count = 1) {
    if (count > 0)
      direct_count_.fetch_add(count, std::memory_order_relaxed);
  }

  void note_worklist(int count = 1) {
    if (count > 0)
      worklist_count_.fetch_add(count, std::memory_order_relaxed);
  }

  void note_tensor(int count = 1) {
    if (count > 0)
      tensor_count_.fetch_add(count, std::memory_order_relaxed);
  }

  void note_tensor_fallback(int count = 1) {
    if (count > 0)
      tensor_fallback_count_.fetch_add(count, std::memory_order_relaxed);
  }

  AdaptiveSccSolveStats snapshot() const {
    AdaptiveSccSolveStats stats;
    stats.used = used_.load(std::memory_order_relaxed);
    stats.direct_count = direct_count_.load(std::memory_order_relaxed);
    stats.worklist_count = worklist_count_.load(std::memory_order_relaxed);
    stats.tensor_count = tensor_count_.load(std::memory_order_relaxed);
    stats.tensor_fallback_count =
        tensor_fallback_count_.load(std::memory_order_relaxed);
    return stats;
  }

private:
  std::atomic<bool> used_{false};
  std::atomic<int> direct_count_{0};
  std::atomic<int> worklist_count_{0};
  std::atomic<int> tensor_count_{0};
  std::atomic<int> tensor_fallback_count_{0};
};

inline ApproximationSourceCollector &npa_default_approximation_collector() {
  static thread_local ApproximationSourceCollector collector;
  return collector;
}

inline AdaptiveSccSolveCollector &npa_default_adaptive_scc_collector() {
  static thread_local AdaptiveSccSolveCollector collector;
  return collector;
}

inline ApproximationSourceCollector *&npa_active_approximation_collector_slot() {
  static thread_local ApproximationSourceCollector *collector = nullptr;
  return collector;
}

inline AdaptiveSccSolveCollector *&npa_active_adaptive_scc_collector_slot() {
  static thread_local AdaptiveSccSolveCollector *collector = nullptr;
  return collector;
}

inline ApproximationSourceCollector &npa_active_approximation_collector() {
  ApproximationSourceCollector *collector =
      npa_active_approximation_collector_slot();
  return collector ? *collector : npa_default_approximation_collector();
}

inline AdaptiveSccSolveCollector &npa_active_adaptive_scc_collector() {
  AdaptiveSccSolveCollector *collector =
      npa_active_adaptive_scc_collector_slot();
  return collector ? *collector : npa_default_adaptive_scc_collector();
}

class ScopedApproximationSourceCollector {
public:
  explicit ScopedApproximationSourceCollector(
      ApproximationSourceCollector &collector)
      : previous_(npa_active_approximation_collector_slot()) {
    npa_active_approximation_collector_slot() = &collector;
  }

  ~ScopedApproximationSourceCollector() {
    npa_active_approximation_collector_slot() = previous_;
  }

private:
  ApproximationSourceCollector *previous_;
};

class ScopedAdaptiveSccSolveCollector {
public:
  explicit ScopedAdaptiveSccSolveCollector(AdaptiveSccSolveCollector &collector)
      : previous_(npa_active_adaptive_scc_collector_slot()) {
    npa_active_adaptive_scc_collector_slot() = &collector;
  }

  ~ScopedAdaptiveSccSolveCollector() {
    npa_active_adaptive_scc_collector_slot() = previous_;
  }

private:
  AdaptiveSccSolveCollector *previous_;
};

template <class D> struct ExecutionContext {
  using domain_state_type = typename DomainExecutionStateTraits<D>::state_type;

  ApproximationSourceCollector *approximation_collector = nullptr;
  domain_state_type domain_state{};
};

template <class D> inline ExecutionContext<D> capture_execution_context() {
  ExecutionContext<D> ctx;
  ctx.approximation_collector = &npa_active_approximation_collector();
  ctx.domain_state = DomainExecutionStateTraits<D>::capture();
  return ctx;
}

template <class D> class ScopedExecutionContext {
public:
  using traits_type = DomainExecutionStateTraits<D>;

  explicit ScopedExecutionContext(const ExecutionContext<D> &ctx)
      : approx_scope_(*ctx.approximation_collector),
        domain_scope_(ctx.domain_state) {}

private:
  ScopedApproximationSourceCollector approx_scope_;
  typename traits_type::scope_type domain_scope_;
};

inline ApproximationSourceFlags npa_approximation_source_flags() {
  return npa_active_approximation_collector().snapshot();
}

inline void npa_reset_limit_hit() {
  npa_active_approximation_collector().reset();
}

inline void npa_note_outer_limit_hit() {
  npa_active_approximation_collector().note_outer_limit_hit();
}

inline void npa_note_linear_limit_hit() {
  npa_active_approximation_collector().note_linear_limit_hit();
}

inline void npa_note_fixpoint_limit_hit() {
  npa_active_approximation_collector().note_fixpoint_limit_hit();
}

inline bool npa_hit_outer_limit() {
  return npa_approximation_source_flags().hit_outer_limit;
}

inline bool npa_hit_linear_limit() {
  return npa_approximation_source_flags().hit_linear_limit;
}

inline bool npa_hit_fixpoint_limit() {
  return npa_approximation_source_flags().hit_fixpoint_limit;
}

inline bool npa_limit_hit() {
  const auto flags = npa_approximation_source_flags();
  return flags.hit_outer_limit || flags.hit_linear_limit ||
         flags.hit_fixpoint_limit;
}

inline AdaptiveSccSolveStats npa_adaptive_scc_solve_stats() {
  return npa_active_adaptive_scc_collector().snapshot();
}

inline void npa_reset_adaptive_scc_stats() {
  npa_active_adaptive_scc_collector().reset();
}

inline void npa_note_adaptive_scc_used() {
  npa_active_adaptive_scc_collector().note_used();
}

inline void npa_note_adaptive_scc_direct(int count = 1) {
  npa_active_adaptive_scc_collector().note_direct(count);
}

inline void npa_note_adaptive_scc_worklist(int count = 1) {
  npa_active_adaptive_scc_collector().note_worklist(count);
}

inline void npa_note_adaptive_scc_tensor(int count = 1) {
  npa_active_adaptive_scc_collector().note_tensor(count);
}

inline void npa_note_adaptive_scc_tensor_fallback(int count = 1) {
  npa_active_adaptive_scc_collector().note_tensor_fallback(count);
}

template <class D>
inline bool valid_newton_delta(const DomVal<D> &f_nu, const DomVal<D> &nu,
                               const DomVal<D> &delta) {
  return domain_exact_equal<D>(D::combine(nu, delta), f_nu);
}

template <class D>
inline bool run_basic_domain_contract_checks(bool verbose = false) {
  bool ok = true;
  if (!D::equal(D::zero(), D::zero())) {
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] zero() must equal itself\n";
  }
  if (!D::equal(D::one(), D::one())) {
    ok = false;
    if (verbose)
      std::cerr << "[npa-contract] one() must equal itself\n";
  }
  if (D::idempotent) {
    if (!D::equal(D::combine(D::zero(), D::zero()), D::zero())) {
      ok = false;
      if (verbose)
        std::cerr << "[npa-contract] idempotent domain: zero⊕zero != zero\n";
    }
    if (!D::equal(D::combine(D::one(), D::one()), D::one())) {
      ok = false;
      if (verbose)
        std::cerr << "[npa-contract] idempotent domain: one⊕one != one\n";
    }
  }
  return ok;
}

class InvalidNewtonDeltaError : public std::logic_error {
public:
  InvalidNewtonDeltaError()
      : std::logic_error("invalid Newton delta: non-idempotent domains must "
                         "provide subtract()/choose_delta() such that "
                         "combine(nu, delta) == f(nu)") {}
};

class UnsupportedNewtonMuError : public std::logic_error {
public:
  UnsupportedNewtonMuError()
      : std::logic_error("unsupported Newton expression: Mu is evaluable but "
                         "outside the paper-faithful Newton/tensor fragment") {}
};

class UnsafeNewtonProjectError : public std::logic_error {
public:
  UnsafeNewtonProjectError()
      : std::logic_error(
            "unsafe Newton projection: domains must opt in with "
            "project_newton_safe for Project on Newton/tensor paths") {}
};

template <class D>
inline void require_valid_newton_delta(const DomVal<D> &f_nu,
                                       const DomVal<D> &nu,
                                       const DomVal<D> &delta) {
  // This check keeps the non-idempotent Newton hook honest: choose_delta() /
  // subtract() may be domain-specific, but they must still produce a residual
  // that exactly reconstructs f(nu) under combine().
  if (valid_newton_delta<D>(f_nu, nu, delta))
    return;
  throw InvalidNewtonDeltaError{};
}

} // namespace npa

#endif // NPA_COMMON_H

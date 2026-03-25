#ifndef NPA_SOLVER_H
#define NPA_SOLVER_H

/**
 * \file
 * \brief Outer iteration: Kleene vs Newton; dispatches linear strategy.
 *
 * Solver<D, ITER> runs ITER until the vector of equation values stabilizes.
 * - KleeneIter: κ^(i+1) = f(κ^(i)) (classical Kleene sequence, Eqn. (1) in
 *   Esparza et al.).
 * - NewtonIter: ν^(0) = f(⊥); ν^(i+1) = ν^(i) ⊔ LinearCorrectionTerm.
 *   The correction is Δ^(i) = least solution of Df|ν^(i)(X) + δ^(i) = X
 *   (Eqn. (2), (13)); then ν^(i+1) = ν^(i) ⊕ Δ^(i) (idempotent) or
 *   ν^(i+1) = ν^(i) + Δ^(i) (non-idempotent). The linear system is solved
 *   by Naive, SCC, AdaptiveScc, or TensorProduct (LinearStrategy).
 *   AdaptiveScc keeps Newton's outer iteration unchanged, but chooses the
 *   inner linear solver independently for each linearized SCC: direct for
 *   singleton acyclic SCCs, SCC worklist for ordinary recursive SCCs, and
 *   tensor solving for tensor-eligible cyclic LCFL SCCs.
 *
 * Exact-vs-approximate status:
 * - `Stat::converged` means theorem-faithful convergence: equality stabilized
 *   and no approximation hook fired.
 * - `used_approx_equal`, `hit_outer_limit`, `hit_linear_limit`, and
 *   `hit_fixpoint_limit` record the approximation sources explicitly.
 * - `hit_limit` remains the aggregate of the bounding hooks only.
 * - `adaptive_scc_*` counters aggregate the SCC-local solver choices made
 *   across all adaptive linear solves in the Newton run, not just the final
 *   converged round.
 *
 * References: Esparza et al. (JACM); Reps et al. (TOPLAS 2016).
 */

#include "Dataflow/NPA/Core/TensorLinearSolve.h"
#include "Utils/Parallel/ThreadPool.h"

#include <exception>

namespace npa {

namespace detail {
enum class NewtonSetupExecutionMode {
  Auto,
  ForceSerial,
  ForceParallel,
};

inline std::size_t newton_parallel_setup_min_equations() {
  return 4;
}

inline bool
should_parallelize_newton_setup(bool verbose, std::size_t equation_count,
                                NewtonSetupExecutionMode mode =
                                    NewtonSetupExecutionMode::Auto) {
  // Phase 2 contract: RHS assembly is parallelized only for non-verbose,
  // single-run setup work. Auto mode preserves the existing zero/one-worker
  // fast path; tests may force either branch through the internal mode knob.
  if (verbose)
    return false;
  if (mode == NewtonSetupExecutionMode::ForceSerial)
    return false;
  if (mode == NewtonSetupExecutionMode::ForceParallel)
    return true;
  if (equation_count < newton_parallel_setup_min_equations())
    return false;
  return ThreadPool::get()->hasWorkers();
}

template <class D>
void collect_polynomial_expr_nodes(const E0<D> &expr,
                                   std::unordered_set<const void *> &nodes) {
  if (!expr)
    return;
  if (!nodes.insert(expr.get()).second)
    return;
  using K = typename Exp0<D>::K;
  switch (expr->k) {
  case K::Seq:
  case K::Call:
  case K::Project:
  case K::Star:
  case K::Mu:
    collect_polynomial_expr_nodes<D>(expr->t, nodes);
    break;
  case K::Mul:
  case K::Cond:
  case K::Ndet:
  case K::Concat:
    collect_polynomial_expr_nodes<D>(expr->t1, nodes);
    collect_polynomial_expr_nodes<D>(expr->t2, nodes);
    break;
  default:
    break;
  }
}

template <class D>
bool equations_share_polynomial_nodes(
    const std::vector<std::pair<Symbol, E0<D>>> &eqns) {
  std::unordered_set<const void *> global_nodes;
  global_nodes.reserve(eqns.size() * 4U);
  for (const auto &eqn : eqns) {
    std::unordered_set<const void *> local_nodes;
    collect_polynomial_expr_nodes<D>(eqn.second, local_nodes);
    for (const void *node : local_nodes) {
      if (global_nodes.count(node))
        return true;
    }
    global_nodes.insert(local_nodes.begin(), local_nodes.end());
  }
  return false;
}

/// C++14-friendly dispatch for delta: avoid if constexpr (DomainHasChooseDelta,
/// idempotent) → choose_delta(v, nu) or v; else subtract(v, nu) or v.
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::true_type /* has_choose_delta */,
                        std::true_type /* idempotent */) {
  (void)nu_sym;
  return v;
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::true_type /* has_choose_delta */,
                        std::false_type /* idempotent */) {
  return D::choose_delta(v, nu_sym);
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::false_type /* has_choose_delta */,
                        std::true_type /* idempotent */) {
  (void)nu_sym;
  return v;
}
template <class D>
DomVal<D> compute_delta(const DomVal<D> &v, const DomVal<D> &nu_sym,
                        std::false_type /* has_choose_delta */,
                        std::false_type /* idempotent */) {
  return D::subtract(v, nu_sym);
}

template <class D> inline void require_newton_compatible_expr(const E0<D> &e) {
  if (ExprFeatureDetector<D>::has_mu(e))
    throw UnsupportedNewtonMuError{};
  if (ExprFeatureDetector<D>::has_project(e) &&
      !domain_project_newton_safe<D>())
    throw UnsafeNewtonProjectError{};
}

template <class D> struct NewtonRoundSetup {
  using tensor_domain = typename TensorSemiringTraits<D>::tensor_domain;
  std::vector<std::pair<Symbol, E1<D>>> rhs;
  std::vector<std::pair<Symbol, E1<tensor_domain>>> rhs_tensor;
  bool has_lcfl_structure = false;
  bool tensor_requested = false;
  bool tensor_available = false;
  bool tensor_admissible = false;
  bool tensor_laws_validated = false;
};

template <class D> struct NewtonRhsTaskRecord {
  using tensor_domain = typename TensorSemiringTraits<D>::tensor_domain;
  E1<D> rhs;
  E1<tensor_domain> rhs_tensor;
  bool has_lcfl_structure = false;
  std::exception_ptr error;
};

template <class D>
std::vector<std::pair<Symbol, DomVal<D>>>
build_newton_initial_values(
    const std::vector<std::pair<Symbol, E0<D>>> &eqns,
    NewtonSetupExecutionMode mode = NewtonSetupExecutionMode::Auto) {
  using V = DomVal<D>;
  std::unordered_map<Symbol, V> nu0;
  for (auto &e : eqns)
    nu0[e.first] = D::zero();

  bool parallelize = should_parallelize_newton_setup(false, eqns.size(), mode);
  // I0 evaluation caches into expression nodes. If equations share AST nodes,
  // stay on the serial path to preserve correctness without synchronization.
  if (parallelize && equations_share_polynomial_nodes<D>(eqns))
    parallelize = false;

  std::vector<std::pair<Symbol, V>> cur;
  cur.reserve(eqns.size());
  if (!parallelize) {
    for (auto &e : eqns) {
      require_newton_compatible_expr<D>(e.second);
      cur.emplace_back(e.first, I0<D>::eval(false, nu0, e.second));
    }
    return cur;
  }

  std::vector<Optional<V>> values(eqns.size());
  std::vector<std::exception_ptr> errors(eqns.size());
  ThreadPool *pool = ThreadPool::get();
  const auto execution_context = capture_execution_context<D>();
  pool->parallelFor<std::size_t>(0, eqns.size(), 1, [&](std::size_t i) {
    ScopedExecutionContext<D> context_scope(execution_context);
    try {
      require_newton_compatible_expr<D>(eqns[i].second);
      values[i] = I0<D>::eval(false, nu0, eqns[i].second);
    } catch (...) {
      errors[i] = std::current_exception();
    }
  });
  for (std::size_t i = 0; i < eqns.size(); ++i) {
    if (errors[i])
      std::rethrow_exception(errors[i]);
    cur.emplace_back(eqns[i].first, *values[i]);
  }
  return cur;
}

template <class D>
NewtonRoundSetup<D> build_newton_round_setup(
    bool verbose, const std::vector<std::pair<Symbol, E0<D>>> &eqns,
    const std::vector<std::pair<Symbol, DomVal<D>>> &binds,
    LinearStrategy linStrat = LinearStrategy::SCC,
    NewtonSetupExecutionMode mode = NewtonSetupExecutionMode::Auto) {
  using V = DomVal<D>;
  using TensorTraits = TensorSemiringTraits<D>;
  using TD = typename TensorTraits::tensor_domain;

  std::unordered_map<Symbol, V> nu;
  for (auto &b : binds)
    nu[b.first] = b.second;

  NewtonRoundSetup<D> setup;
  setup.tensor_requested = linStrat == LinearStrategy::TensorProduct ||
                           linStrat == LinearStrategy::AdaptiveScc;
  setup.tensor_available = setup.tensor_requested && TensorTraits::available();
  setup.tensor_admissible =
      setup.tensor_available && TensorTraits::paper_admissible();
  setup.tensor_laws_validated =
      setup.tensor_admissible && tensor_paper_laws_validated<D>();

  bool parallelize = should_parallelize_newton_setup(verbose, eqns.size(), mode);
  // The Newton setup path assumes per-equation AST ownership during cached
  // evaluation. Shared polynomial nodes force a conservative serial fallback.
  if (parallelize && equations_share_polynomial_nodes<D>(eqns))
    parallelize = false;

  setup.rhs.reserve(eqns.size());
  if (setup.tensor_laws_validated)
    setup.rhs_tensor.reserve(eqns.size());

  auto build_eqn_rhs = [&](std::size_t index, bool eval_verbose,
                           NewtonRhsTaskRecord<D> &record) {
    const auto &eqn = eqns[index];
    require_newton_compatible_expr<D>(eqn.second);
    V v = I0<D>::eval(eval_verbose, nu, eqn.second);
    V delta0 = compute_delta<D>(
        v, nu[eqn.first],
        std::integral_constant<bool, DomainHasChooseDelta<D>::value>{},
        std::integral_constant<bool, D::idempotent>{});
    if (!D::idempotent)
      require_valid_newton_delta<D>(v, nu[eqn.first], delta0);
    auto d = Diff<D>::build(nu, eqn.second);
    record.has_lcfl_structure = LCFLDetector<D>::has_lcfl_structure(d);
    record.rhs = Exp1<D>::add(Exp1<D>::term(delta0), d);
    if (setup.tensor_laws_validated) {
      auto tensor_d = TensorDiff<D>::build(nu, eqn.second);
      E1<TD> tensor_rhs = Exp1<TD>::add(
          Exp1<TD>::term(TensorTraits::right_constant(delta0)), tensor_d);
      if (tensor_supports_projection_equations<D>() && eqn.second &&
          eqn.second->k == Exp0<D>::Project && tensor_d &&
          tensor_d->k == Exp1<TD>::Project) {
        tensor_rhs = Exp1<TD>::project(Exp1<TD>::add(
            Exp1<TD>::term(TensorTraits::right_constant(delta0)),
            tensor_d->t));
      }
      record.rhs_tensor = tensor_rhs;
    }
  };

  if (!parallelize) {
    for (std::size_t i = 0; i < eqns.size(); ++i) {
      NewtonRhsTaskRecord<D> record;
      build_eqn_rhs(i, verbose, record);
      setup.has_lcfl_structure =
          setup.has_lcfl_structure || record.has_lcfl_structure;
      setup.rhs.emplace_back(eqns[i].first, record.rhs);
      if (setup.tensor_laws_validated)
        setup.rhs_tensor.emplace_back(eqns[i].first, record.rhs_tensor);
    }
    return setup;
  }

  std::vector<NewtonRhsTaskRecord<D>> records(eqns.size());
  ThreadPool *pool = ThreadPool::get();
  const auto execution_context = capture_execution_context<D>();
  pool->parallelFor<std::size_t>(0, eqns.size(), 1, [&](std::size_t i) {
    ScopedExecutionContext<D> context_scope(execution_context);
    try {
      build_eqn_rhs(i, false, records[i]);
    } catch (...) {
      records[i].error = std::current_exception();
    }
  });

  setup.has_lcfl_structure = pool->parallelReduce<std::size_t>(
      0, records.size(), 1, false,
      [&records](std::size_t i) {
        return records[i].has_lcfl_structure;
      },
      [](bool has_lcfl, bool local_has_lcfl) {
        return has_lcfl || local_has_lcfl;
      });

  for (std::size_t i = 0; i < eqns.size(); ++i) {
    if (records[i].error)
      std::rethrow_exception(records[i].error);
    setup.rhs.emplace_back(eqns[i].first, records[i].rhs);
    if (setup.tensor_laws_validated)
      setup.rhs_tensor.emplace_back(eqns[i].first, records[i].rhs_tensor);
  }
  return setup;
}

template <class D>
bool tensor_expr_is_projection_sensitive(
    const E1<typename TensorSemiringTraits<D>::tensor_domain> &expr) {
  using TD = typename TensorSemiringTraits<D>::tensor_domain;
  return ExprFeatureDetector<TD>::has_project(expr) &&
         !Exp1ConstEval<TD>::eval(expr).has_value();
}

template <class D>
void annotate_adaptive_scc_plan(
    detail::LinearSccPlan<D> &plan,
    const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    const NewtonRoundSetup<D> &setup) {
  const bool projection_fragment_supported =
      tensor_supports_projection_equations<D>();
  for (std::size_t sid = 0; sid < plan.infos.size(); ++sid) {
    auto &info = plan.infos[sid];
    info.tensor_available = setup.tensor_available;
    info.tensor_admissible = setup.tensor_admissible;
    info.tensor_laws_validated = setup.tensor_laws_validated;
    info.tensor_projection_fragment_supported = projection_fragment_supported;
    for (int idx : info.members) {
      info.has_lcfl_structure =
          info.has_lcfl_structure ||
          LCFLDetector<D>::has_lcfl_structure(rhs[static_cast<std::size_t>(idx)]
                                                  .second);
      if (setup.tensor_laws_validated) {
        info.tensor_projection_sensitive =
            info.tensor_projection_sensitive ||
            tensor_expr_is_projection_sensitive<D>(
                rhs_tensor[static_cast<std::size_t>(idx)].second);
      }
    }

    const bool tensor_candidate = info.is_cyclic && info.has_lcfl_structure;
    if (tensor_candidate) {
      if (!setup.tensor_available) {
        info.tensor_fallback = true;
        info.tensor_fallback_reason =
            detail::TensorFallbackReason::TensorUnavailable;
      } else if (!setup.tensor_admissible) {
        info.tensor_fallback = true;
        info.tensor_fallback_reason =
            detail::TensorFallbackReason::TensorNotPaperAdmissible;
      } else if (!setup.tensor_laws_validated) {
        info.tensor_fallback = true;
        info.tensor_fallback_reason =
            detail::TensorFallbackReason::TensorLawsNotValidated;
      } else if (info.tensor_projection_sensitive &&
                 !projection_fragment_supported) {
        info.tensor_fallback = true;
        info.tensor_fallback_reason =
            detail::TensorFallbackReason::ProjectionFragmentUnsupported;
      } else {
        info.tensor_eligible = true;
      }
    }

    if (info.members.size() == 1 && !info.has_self_loop) {
      info.strategy = detail::SccStrategy::Direct;
    } else if (info.tensor_eligible) {
      info.strategy = detail::SccStrategy::Tensor;
    } else {
      info.strategy = detail::SccStrategy::Worklist;
    }
  }
}

template <class D>
void solve_linear_direct_component(
    const std::vector<std::pair<Symbol, E1<D>>> &rhs, const int idx,
    const std::unordered_map<Symbol, DomVal<D>> &base_env,
    std::atomic<long> &shared_steps, detail::LinearSccTaskResult<D> &result) {
  result.values.clear();
  const Symbol &sym = rhs[static_cast<std::size_t>(idx)].first;
  if (!detail::try_claim_linear_step(shared_steps, domain_max_linear_steps<D>())) {
    result.hit_limit = true;
    result.values.push_back(base_env.at(sym));
    return;
  }
  ++result.steps;
  result.values.push_back(I1<D>::eval(
      false, base_env, rhs[static_cast<std::size_t>(idx)].second));
}

template <class D>
void solve_linear_tensor_component(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    const std::vector<int> &scc,
    const std::unordered_map<Symbol, DomVal<D>> &base_env,
    detail::LinearSccTaskResult<D> &result) {
  using TD = typename TensorSemiringTraits<D>::tensor_domain;
  std::vector<std::pair<Symbol, E1<D>>> local_rhs;
  std::vector<std::pair<Symbol, E1<TD>>> local_rhs_tensor;
  std::vector<DomVal<D>> init;
  local_rhs.reserve(scc.size());
  local_rhs_tensor.reserve(scc.size());
  init.reserve(scc.size());
  for (int idx : scc) {
    local_rhs.push_back(rhs[static_cast<std::size_t>(idx)]);
    local_rhs_tensor.push_back(rhs_tensor[static_cast<std::size_t>(idx)]);
    const Symbol &sym = rhs[static_cast<std::size_t>(idx)].first;
    init.push_back(base_env.at(sym));
  }
  result.values =
      solve_linear_tensor_paper_impl<D>(verbose, local_rhs, local_rhs_tensor,
                                        std::move(init));
}

template <class D>
std::vector<DomVal<D>> solve_linear_adaptive_scc_from_plan(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    std::vector<DomVal<D>> init, const detail::LinearSccPlan<D> &plan) {
  using V = DomVal<D>;
  std::unordered_map<Symbol, V> env;
  for (int i = 0; i < static_cast<int>(rhs.size()); ++i)
    env[rhs[static_cast<std::size_t>(i)].first] = init[static_cast<std::size_t>(i)];

  std::atomic<long> shared_steps(0);
  ThreadPool *pool = ThreadPool::get();
  const bool parallelize = detail::should_parallelize_linear_scc(verbose, plan);
  const auto execution_context = capture_execution_context<D>();

  auto note_layer_stats = [&](const std::vector<int> &layer) {
    struct AdaptiveSccStats {
      int direct_count = 0;
      int worklist_count = 0;
      int tensor_count = 0;
      int tensor_fallback_count = 0;
    };

    AdaptiveSccStats stats = pool->parallelReduce<std::size_t>(
        0, layer.size(), 1, AdaptiveSccStats(),
        [&](std::size_t pos) {
          AdaptiveSccStats local;
          const auto &info =
              plan.infos[static_cast<std::size_t>(layer[pos])];
          switch (info.strategy) {
          case detail::SccStrategy::Direct:
            ++local.direct_count;
            break;
          case detail::SccStrategy::Worklist:
            ++local.worklist_count;
            break;
          case detail::SccStrategy::Tensor:
            ++local.tensor_count;
            break;
          }
          if (info.tensor_fallback)
            ++local.tensor_fallback_count;
          return local;
        },
        [](AdaptiveSccStats acc, const AdaptiveSccStats &value) {
          acc.direct_count += value.direct_count;
          acc.worklist_count += value.worklist_count;
          acc.tensor_count += value.tensor_count;
          acc.tensor_fallback_count += value.tensor_fallback_count;
          return acc;
        });

    npa_note_adaptive_scc_used();
    npa_note_adaptive_scc_direct(stats.direct_count);
    npa_note_adaptive_scc_worklist(stats.worklist_count);
    npa_note_adaptive_scc_tensor(stats.tensor_count);
    npa_note_adaptive_scc_tensor_fallback(stats.tensor_fallback_count);
  };

  auto solve_component = [&](int sid, detail::LinearSccTaskResult<D> &result) {
    const auto &info = plan.infos[static_cast<std::size_t>(sid)];
    const auto &scc = plan.sccs[static_cast<std::size_t>(sid)];
    switch (info.strategy) {
    case detail::SccStrategy::Direct:
      solve_linear_direct_component<D>(rhs, scc.front(), env, shared_steps,
                                       result);
      break;
    case detail::SccStrategy::Tensor:
      solve_linear_tensor_component<D>(verbose, rhs, rhs_tensor, scc, env,
                                       result);
      break;
    case detail::SccStrategy::Worklist:
      detail::solve_linear_scc_parallel_component<D>(
          rhs, scc, plan.intra_scc_users, env, shared_steps, result);
      break;
    }
  };

  for (const auto &layer : plan.layers) {
    std::vector<detail::LinearSccTaskResult<D>> results(layer.size());
    if (parallelize && layer.size() > 1) {
      pool->parallelFor<std::size_t>(0, layer.size(), 1,
                                     [&](std::size_t pos) {
        const int sid = layer[pos];
        ScopedExecutionContext<D> context_scope(execution_context);
        try {
          solve_component(sid, results[pos]);
        } catch (...) {
          results[pos].error = std::current_exception();
        }
      });
    } else {
      for (std::size_t pos = 0; pos < layer.size(); ++pos) {
        try {
          solve_component(layer[pos], results[pos]);
        } catch (...) {
          results[pos].error = std::current_exception();
        }
      }
    }

    for (std::size_t pos = 0; pos < layer.size(); ++pos) {
      if (results[pos].error)
        std::rethrow_exception(results[pos].error);
      const int sid = layer[pos];
      const auto &scc = plan.sccs[static_cast<std::size_t>(sid)];
      for (std::size_t value_pos = 0; value_pos < scc.size(); ++value_pos) {
        const int idx = scc[value_pos];
        env[rhs[static_cast<std::size_t>(idx)].first] =
            results[pos].values[value_pos];
        init[static_cast<std::size_t>(idx)] = results[pos].values[value_pos];
      }
    }
    const bool hit_limit = pool->parallelReduce<std::size_t>(
        0, results.size(), 1, false,
        [&results](std::size_t pos) { return results[pos].hit_limit; },
        [](bool acc, bool value) { return acc || value; });
    note_layer_stats(layer);
    if (hit_limit) {
      npa_note_linear_limit_hit();
      return init;
    }
  }

  return init;
}

template <class D>
std::vector<DomVal<D>> solve_linear_adaptive_scc_impl(
    bool verbose, const std::vector<std::pair<Symbol, E1<D>>> &rhs,
    const std::vector<
        std::pair<Symbol, E1<typename TensorSemiringTraits<D>::tensor_domain>>>
        &rhs_tensor,
    std::vector<DomVal<D>> init, const NewtonRoundSetup<D> &setup) {
  auto plan = detail::build_linear_scc_plan<D>(rhs);
  annotate_adaptive_scc_plan<D>(plan, rhs, rhs_tensor, setup);
  return solve_linear_adaptive_scc_from_plan<D>(verbose, rhs, rhs_tensor,
                                                std::move(init), plan);
}

template <class D>
std::vector<std::pair<Symbol, DomVal<D>>> run_newton_iteration(
    bool verbose, const std::vector<std::pair<Symbol, E0<D>>> &eqns,
    const std::vector<std::pair<Symbol, DomVal<D>>> &binds,
    LinearStrategy linStrat = LinearStrategy::SCC,
    NewtonSetupExecutionMode mode = NewtonSetupExecutionMode::Auto) {
  using V = DomVal<D>;
  auto setup = build_newton_round_setup<D>(verbose, eqns, binds, linStrat, mode);
  const bool use_tensor =
      setup.tensor_laws_validated && setup.has_lcfl_structure;
  if (linStrat == LinearStrategy::TensorProduct && verbose) {
    if (!TensorSemiringTraits<D>::available()) {
      std::cerr << "[tensor] tensor traits unavailable for domain; "
                   "falling back to SCC\n";
    } else if (!setup.has_lcfl_structure) {
      std::cerr << "[tensor] linearized system is already left-linear; "
                   "falling back to SCC\n";
    } else if (!TensorSemiringTraits<D>::paper_admissible()) {
      std::cerr << "[tensor] tensor traits are not paper-admissible; "
                   "falling back to SCC\n";
    } else if (!tensor_paper_laws_validated<D>()) {
      std::cerr << "[tensor] tensor traits did not pass/declare paper-law "
                   "validation; falling back to SCC\n";
    }
  }

  std::vector<V> init(use_tensor ? setup.rhs_tensor.size() : setup.rhs.size(),
                      D::zero()),
      delta;
  if (linStrat == LinearStrategy::Naive) {
    delta = fix_vec<D>(verbose, init, [&](const std::vector<V> &cur) {
      std::unordered_map<Symbol, V> env;
      for (size_t i = 0; i < cur.size(); ++i)
        env[setup.rhs[i].first] = cur[i];
      std::vector<V> nxt;
      nxt.reserve(setup.rhs.size());
      for (auto &p : setup.rhs)
        nxt.push_back(I1<D>::eval(false, env, p.second));
      return nxt;
    });
  } else if (linStrat == LinearStrategy::SCC) {
    delta = solve_linear_scc_impl<D>(verbose, setup.rhs, init);
  } else if (linStrat == LinearStrategy::AdaptiveScc) {
    delta = solve_linear_adaptive_scc_impl<D>(verbose, setup.rhs,
                                              setup.rhs_tensor, init, setup);
  } else if (use_tensor) {
    delta = solve_linear_tensor_paper_impl<D>(verbose, setup.rhs,
                                              setup.rhs_tensor, init);
  } else {
    delta = solve_linear_scc_impl<D>(verbose, setup.rhs, init);
  }

  std::vector<std::pair<Symbol, V>> out;
  out.reserve(binds.size());
  for (size_t i = 0; i < binds.size(); ++i) {
    V upd = delta[i];
    V nxt = D::idempotent ? upd : D::combine(binds[i].second, upd);
    out.emplace_back(binds[i].first, nxt);
  }
  return out;
}
} // namespace detail

template <class D, class ITER> struct Solver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solve(const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
        LinearStrategy linStrat = LinearStrategy::SCC,
        DomainContractMode contractMode = DomainContractMode::Off) {
    NPA_REQUIRE_DOMAIN(D);
    ApproximationSourceCollector approximation_collector;
    ScopedApproximationSourceCollector collector_scope(approximation_collector);
    AdaptiveSccSolveCollector adaptive_scc_collector;
    ScopedAdaptiveSccSolveCollector adaptive_scc_scope(adaptive_scc_collector);
    npa_reset_limit_hit();
    npa_reset_adaptive_scc_stats();
    bool contractOk = true;
    const bool checksRun = contractMode == DomainContractMode::BasicChecks;
    if (checksRun) {
      contractOk = run_basic_domain_contract_checks<D>(verbose);
    }
    std::vector<std::pair<Symbol, V>> cur = ITER::init(eqns);
    auto tic = std::chrono::high_resolution_clock::now();
    int it = 0;
    bool converged = false;
    while (max < 0 || it < max) {
      auto nxt = ITER::run(verbose, eqns, cur, linStrat);
      bool stable = true;
      for (size_t i = 0; i < cur.size(); ++i)
        if (!domain_equal<D>(cur[i].second, nxt[i].second)) {
          stable = false;
          break;
        }
      cur.swap(nxt);
      ++it;
      if (stable) {
        converged = true;
        if (verbose)
          std::cerr << "[conv] " << it << "\n";
        break;
      }
    }
    const bool hit_outer_limit = !converged && max >= 0 && it >= max;
    if (hit_outer_limit) {
      npa_note_outer_limit_hit();
      if (verbose)
        std::cerr << "[conv] hit outer iteration cap=" << max << "\n";
    }
    auto toc = std::chrono::high_resolution_clock::now();
    Stat st;
    st.iters = it;
    st.time = std::chrono::duration<double>(toc - tic).count();
    st.hit_limit = npa_limit_hit();
    st.hit_outer_limit = npa_hit_outer_limit();
    st.hit_linear_limit = npa_hit_linear_limit();
    st.hit_fixpoint_limit = npa_hit_fixpoint_limit();
    st.equation_count = static_cast<int>(eqns.size());
    st.requested_max_iters = max;
    st.effective_max_iters = max;
    st.linear_strategy = linStrat;
    st.used_approx_equal = DomainHasApproxEqual<D>::value;
    const auto adaptive_stats = npa_adaptive_scc_solve_stats();
    st.adaptive_scc_used = adaptive_stats.used;
    st.adaptive_scc_direct_count = adaptive_stats.direct_count;
    st.adaptive_scc_worklist_count = adaptive_stats.worklist_count;
    st.adaptive_scc_tensor_count = adaptive_stats.tensor_count;
    st.adaptive_scc_tensor_fallback_count =
        adaptive_stats.tensor_fallback_count;
    st.converged = converged && !st.hit_limit && !st.used_approx_equal;
    st.domain_contract_checks_run = checksRun;
    st.domain_contract_checks_failed = checksRun && !contractOk;
    return {cur, st};
  }
};

/// Kleene iteration: one round = evaluate all equations under current ν.
/// κ^(i+1) = f(κ^(i)); no linear correction (Esparza et al. Eqn. (1)).
template <class D> struct KleeneIter {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::vector<std::pair<Symbol, V>> init(const std::vector<Eqn> &eqns) {
    std::vector<std::pair<Symbol, V>> cur;
    cur.reserve(eqns.size());
    for (auto &e : eqns)
      cur.emplace_back(e.first, D::zero());
    return cur;
  }
  static std::vector<std::pair<Symbol, V>>
  run(bool verbose, const std::vector<Eqn> &eqns,
      const std::vector<std::pair<Symbol, V>> &binds,
      LinearStrategy = LinearStrategy::SCC) {
    std::unordered_map<Symbol, V> nu;
    for (auto &b : binds)
      nu[b.first] = b.second;
    std::vector<std::pair<Symbol, V>> out;
    for (auto &e : eqns)
      out.emplace_back(e.first, I0<D>::eval(verbose, nu, e.second));
    return out;
  }
};

/// Newton iteration: one round = f(ν) plus least solution of Df|ν(X)+δ = X.
/// δ = f(ν)−ν (or f(ν) when idempotent); Δ = solve linear system; ν' = ν⊕Δ.
///
/// This is the paper-faithful core when the domain uses exact equality and the
/// selected linear solver reaches the least solution without hitting any
/// bounding hooks. `approx_equal`, bounded fixpoint/linear hooks, or explicit
/// outer caps intentionally move the result into the approximate mode recorded
/// in `Stat`. Tensor mode is only used through paper-admissible traits;
/// otherwise the implementation deliberately falls back to the base solver.
template <class D> struct NewtonIter {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::vector<std::pair<Symbol, V>> init(const std::vector<Eqn> &eqns) {
    return detail::build_newton_initial_values<D>(eqns);
  }
  static std::vector<std::pair<Symbol, V>>
  run(bool verbose, const std::vector<Eqn> &eqns,
      const std::vector<std::pair<Symbol, V>> &binds,
      LinearStrategy linStrat = LinearStrategy::SCC) {
    return detail::run_newton_iteration<D>(verbose, eqns, binds, linStrat);
  }
};

template <class D> using KleeneSolver = Solver<D, KleeneIter<D>>;
template <class D> struct NewtonSolver {
  using V = DomVal<D>;
  using Eqn = std::pair<Symbol, E0<D>>;
  static std::pair<std::vector<std::pair<Symbol, V>>, Stat>
  solve(const std::vector<Eqn> &eqns, bool verbose = false, int max = -1,
        LinearStrategy linStrat = LinearStrategy::SCC,
        DomainContractMode contractMode = DomainContractMode::Off) {
    // JACM (Esparza et al.) shows: for idempotent + commutative semirings,
    // Newton terminates after at most n iterations for a system of n equations.
    // We only apply this bound when the domain explicitly declares
    // commutativity. If that declared contract is insufficient in practice,
    // we continue uncapped rather than silently returning a bounded result.
    int effective_max = max;
    const bool auto_cap =
        effective_max < 0 && D::idempotent && domain_commutative_extend<D>();
    if (auto_cap) {
      effective_max = static_cast<int>(eqns.size());
    }
    auto res = Solver<D, NewtonIter<D>>::solve(eqns, verbose, effective_max,
                                               linStrat, contractMode);
    res.second.used_auto_n_cap = auto_cap;
    res.second.effective_max_iters = effective_max;
    if (auto_cap && !res.second.converged) {
      if (verbose)
        std::cerr << "[conv] automatic n-iteration bound was insufficient; "
                     "continuing without the cap\n";
      res = Solver<D, NewtonIter<D>>::solve(eqns, verbose, -1, linStrat,
                                            contractMode);
      res.second.used_auto_n_cap = true;
      res.second.retried_without_auto_n_cap = true;
    }
    return res;
  }
};

} // namespace npa

#endif // NPA_SOLVER_H

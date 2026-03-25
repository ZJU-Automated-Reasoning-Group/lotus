# Newtonian Program Analysis (NPA)

NPA implements Newton-style program analysis over ω-continuous semirings, following the algorithms in Esparza et al. (JACM) and Reps et al. (TOPLAS 2016).
The primary target is **idempotent** semirings; non-idempotent domains are supported when `subtract`/`choose_delta`
returns a valid Newton residual `delta` satisfying `combine(nu, delta) == f(nu)`.
For numeric semirings where exact equality is too strong, a domain may optionally provide `approx_equal(a,b)`; solvers will use it when present.

## Algorithm overview

1. **Equation system**: Interprocedural dataflow is formulated as \( \vec{X} = \vec{f}(\vec{X}) \) over a semiring (combine ⊕, extend ⊗, zero ⊥, one 1).

2. **Kleene iteration** (classical): \( \vec{\kappa}^{(0)} = \vec{\bot} \), \( \vec{\kappa}^{(i+1)} = \vec{f}(\vec{\kappa}^{(i)}) \).

3. **Newton iteration** (Esparza et al.): \( \vec{\nu}^{(i+1)} = \vec{\nu}^{(i)} \sqcup \Delta^{(i)} \), where \( \Delta^{(i)} \) is the *least solution* of the *linearized* system:
   \[
   D\vec{f}|_{\vec{\nu}^{(i)}}(\vec{X}) + \vec{\delta}^{(i)} = \vec{X}.
   \]
   The differential \( D\vec{f}|_{\vec{\nu}} \) linearizes \( \vec{f} \) at \( \vec{\nu} \); when ⊗ is non-commutative, this system is an **LCFL equation system** (coefficients on both sides of variables).

4. **Solving the linear system** (each Newton round):
   - **SCC**: Global SCC scheduling with dependency-driven local worklists on the linear RHS.
   - **Adaptive SCC**: Decompose the linearized system into SCCs, classify each SCC independently, then choose a local exact solver per SCC: direct evaluation for singleton acyclic SCCs, SCC worklist for ordinary recursive SCCs, and tensor solving for tensor-eligible cyclic LCFL SCCs.
   - **Tensor product** (Reps et al., Alg. 3.4): Convert LCFL system to a tensorized left-linear system, solve there as a regular path problem, then apply tensor-side readout to project back to the base semiring. The implementation uses Tarjan path expressions when the tensorized system can be extracted into a left-linear labeled graph, and otherwise falls back to tensor-space worklist iteration.

## Implementation alignment with the papers

### Paper-faithful core

- **Kleene**: \( \kappa^{(i+1)} = f(\kappa^{(i)}) \) — implemented as `KleeneIter` (evaluate all equations under current \( \nu \)).
- **Newton**: \( \nu^{(i+1)} = \nu^{(i)} \sqcup \Delta^{(i)} \), \( \Delta^{(i)} \) = least solution of \( Df|_{\nu^{(i)}}(X) + \delta^{(i)} = X \) — implemented as `NewtonIter`: build RHS = \( \delta + Df|_\nu(X) \) (with \( \delta = f(\nu)-\nu \) or \( f(\nu) \) when idempotent), solve the linear system, then update with the solved correction. For idempotent domains this coincides with Proposition 7.1, so the next approximant is exactly the solved linear-system result when no approximation hook fires.
- **Initial value**: The code uses \( \nu^{(0)} = f(\bot) \) (as in Esparza et al.).
- **Expression fragment**: `Star` is the paper-faithful Kleene-star construct used by Newton/tensor regularization; `Mu` is a generic least-fixpoint construct that is evaluable but intentionally rejected on Newton/tensor paths.
- **Differential** (Esparza et al. Defn. 3.1, 3.5, plus TOPLAS 2016 Sec. 6.2): Term→0, Seq→c·d(t), Call→\( \nu(f)\cdot d(arg) + f(\nu(arg)) \), Cond/Ndet by linearity, Hole→X, Bound→0, **Concat**→\( D(t_1)\cdot \nu_X\cdot t_2 + t_1\cdot X\cdot t_2 + t_1\cdot \nu_X\cdot D(t_2) \), **Star**→\( g(\nu)^* \cdot D(g) \cdot g(\nu)^* \). Tensor mode uses the corresponding tensored rule from Sec. 6.2.
- **Tensor differential**: `TensorDiff.h` exposes a direct tensor-side differential builder, so tensor mode no longer needs to be expressed as ordinary differential plus post-hoc conversion.
- **Tensor product** (Reps et al. Alg. 3.4): `TensorSemiringTraits<D>` defines the tensor semiring, coefficient coupling, and tensor-side readout for a base domain. The high-level `TensorProduct` strategy only uses traits that explicitly report `paper_admissible() == true`.
- **Adaptive SCC solving**: `LinearStrategy::AdaptiveScc` keeps Newton's outer iteration unchanged, but treats each linearized SCC as the unit of solver choice. Tensor reasoning is local to the eligible SCCs rather than global to the whole linearized system.
- **Plan reuse**: the solver caches both ordinary differential shape plans and tensor dependency-graph regex topology across Newton rounds; current-round coefficients are still re-evaluated from the current Newton iterate.
- **Linearized evaluation**: `Exp1` evaluation uses `extend_lin`, so domains can specialize linearized composition separately from full-summary composition if needed.
- **Projection safety**: `Project` is allowed on Newton/tensor paths only for domains that explicitly opt in via `project_newton_safe`; plain `project()` support is not enough.

### Supported extensions beyond the papers

- `approx_equal(a, b)` is an optional convergence hook for numeric domains. It is a pragmatic stability extension and moves the solver into approximate mode even when the iterates stabilize.
- `max_fixpoint_iters` and `max_linear_steps` intentionally turn `Star`/`Mu` evaluation or linear-system solving into bounded approximations. The exact source is reported via `Stat::hit_fixpoint_limit` / `Stat::hit_linear_limit`.
- `SummaryTransformerDomain` provides a bounded abstract-summary path for the current in-tree subdistributive analyses. It is an over-approximate engineering realization inspired by the Section 8 goal, not a paper-faithful realization of JACM Sections 3 or 7.
- `ProgramTransferDomain` remains available for older tests and utility clients that still want explicit path-set summaries.
- The automatic `n`-iteration cap for commutative idempotent domains is a convenience optimization derived from the JACM theorem. If the declared domain traits are not sufficient in practice, the solver falls back to an uncapped run instead of silently returning a truncated result.

### Deliberate non-paper fallbacks and boundaries

- Domains may specialize `TensorSemiringTraits<D>` for experimental or utility tensor domains. These can still be useful in low-level tests, but only `paper_admissible()` traits participate in the high-level `TensorProduct` solver path.
- When tensor regularization cannot be applied cleanly, the implementation falls back to the base-domain SCC solver rather than attempting a partial paper construction.
- In `AdaptiveScc`, tensor rejection is recorded per SCC and the affected SCC falls back to the ordinary SCC-local worklist solver. This fallback remains exact unless other approximation hooks fire.
- Interprocedural constant propagation and interval analysis use the abstract-summary path (`SummaryTransformerDomain`) instead of `ProgramTransferDomain` in their public APIs; these clients are deliberately approximate and surface summary-overflow / widening in `AnalysisStatus`.
- The interprocedural engines use a closed-world, whole-module assumption for indirect calls: unknown call targets are approximated by type-compatible defined functions in the current LLVM module unless a stronger resolver is supplied outside NPA.
- Non-idempotent Newton support exists at the core API level through `subtract` / `choose_delta`, but the current in-tree analyses are still centered on idempotent domains.

## Soundness modes matrix

| Axis | Mode / Hook | Meaning | Status signal |
|---|---|---|---|
| Equality | `equal` (default) | Exact semiring equality for convergence checks | `Stat::used_approx_equal == false` |
| Equality | `approx_equal` (optional) | Pragmatic convergence hook for numeric domains; stabilization is approximate rather than theorem-exact | `Stat::used_approx_equal == true`, `Stat::converged == false` |
| Outer bound | explicit `max` in solver API | User-requested outer cap on solver rounds | `Stat::requested_max_iters`, `Stat::effective_max_iters`, `Stat::hit_outer_limit` |
| Outer bound | automatic `n` cap (`idempotent && commutative_extend`) | Convenience optimization from JACM bound; solver retries uncapped if cap was insufficient | `Stat::used_auto_n_cap`, `Stat::retried_without_auto_n_cap` |
| Domain checks | `DomainContractMode::BasicChecks` | Lightweight runtime domain sanity probes (development/debug aid) | `Stat::domain_contract_checks_run`, `Stat::domain_contract_checks_failed` |
| Indirect calls | `ClosedWorldTypeCompatible` | Resolve indirect calls using type-compatible defined functions in module | `AnalysisStatus::open_world_unsound_mode == true` |
| Indirect calls | `DeclaredOnlyFallback` | Do not synthesize indirect targets; use fallback/call-to-return behavior | `AnalysisStatus::unresolved_indirect_calls` |
| Indirect calls | `CustomResolverRequired` | Require analysis-provided resolver for indirect calls; unresolved calls are reported | `AnalysisStatus::requires_external_callee_resolver` |
| Bounded inner solve | `max_fixpoint_iters`, `max_linear_steps` | Converts exact least-solution target into bounded approximation when cap hits | `Stat::hit_fixpoint_limit`, `Stat::hit_linear_limit`, `AnalysisStatus::used_bounded_inner_solve` |
| Adaptive SCC diagnostics | `LinearStrategy::AdaptiveScc` | Records how many SCC-local solves used direct/worklist/tensor and how often tensor eligibility fell back | `Stat::adaptive_scc_*` |
| Section 8-style clients | summary overflow / widening | CP and interval clients lose exactness through bounded summaries and widening | `AnalysisStatus::used_summary_overflow`, `AnalysisStatus::used_fact_widening`, `AnalysisStatus::approximated` |

## Domain author checklist

- Provide a semiring implementation satisfying the NPA contract (`zero`, `one`, `combine`, `extend`, `extend_lin`, `ndetCombine`, `condCombine`, `equal`).
- For non-idempotent domains, provide `subtract` or `choose_delta` such that `combine(nu, delta) == f(nu)`.
- If enabling `approx_equal`, document tolerance semantics and accept that convergence is pragmatic, not theorem-exact.
- If enabling `project_newton_safe`, ensure projection is compatible with Newton linearization and summary composition used by your domain.
- If enabling tensor traits, ensure `paper_admissible()` and projection/readout obligations hold for your domain.
- If enabling bounded hooks (`max_fixpoint_iters` / `max_linear_steps`), treat results as approximations and propagate status to callers.

## References

- **Esparza et al.**, "Newtonian Program Analysis", JACM.  
  Differential \( Df|_{\nu} \), Newton sequence, convergence to least fixed point.
- **Reps et al.**, "Newtonian Program Analysis via Tensor Product", TOPLAS 2016.  
  LCFL sub-problems, regularization via tensor product (Defn. 3.1, Alg. 3.4).

## Structure

```
include/Dataflow/NPA/
├── NPA.h                      # Umbrella header; Kleene/Newton, paper refs
├── Core/                      # Core algorithms (see NPA.h for file roles)
│   ├── NPACommon.h            # Domain concept, LinearStrategy
│   ├── Expressions.h          # Exp0 (polynomial) / Exp1 (linearized)
│   ├── Fixpoint.h             # fix / fix_vec
│   ├── Eval.h                 # I0 / I1 evaluators
│   ├── Diff.h                 # Differential Df|ν
│   ├── TensorDiff.h           # Tensor-side differential
│   ├── LCFLDetector.h         # LCFL structure (Concat/Star)
│   ├── LinearSolvers.h        # SCC-based and tensor solvers
│   ├── TensorSemiring.h       # Tensor semiring/readout traits
│   ├── TensorLinearSolve.h    # Tensor-product solver (Alg. 3.4)
│   └── Solver.h               # KleeneIter / NewtonIter
├── Domains/
│   ├── BitVectorDomain.h
│   ├── BitVectorInfo.h
│   ├── GenKillDomain.h
│   ├── SummaryTransformerDomain.h
│   ├── TaintTransferDomain.h
│   └── TensorProductDomain.h  # Paired semiring (TOPLAS 2016)
└── Analyses/
    ├── BitVectorSolver.h
    ├── BackwardInterproceduralEngine.h
    ├── InterproceduralEngine.h
    ├── Intraprocedural/
    │   ├── LiveVariables.h
    │   ├── ReachingDefinitions.h
    │   └── ReachableBlocks.h
    └── Interprocedural/
        ├── InterproceduralAffineEqualities.h
        ├── InterproceduralConstantPropagation.h
        ├── InterproceduralIntervalAnalysis.h
        ├── InterproceduralLiveVariables.h
        ├── InterproceduralMaybeUninitialized.h
        ├── InterproceduralNullability.h
        ├── InterproceduralRD.h
        └── InterproceduralTaint.h
```

## Usage

- **Intraprocedural**: local clients such as `BitVectorSolver` can use conventional worklist/Kleene-style solving.
- **Interprocedural (forward)**: `InterproceduralEngine<Domain, Analysis>` builds recursive procedure-summary equations and reports merged valid-path facts at blocks.
- **Interprocedural (backward)**: `BackwardInterproceduralEngine<Domain, Analysis>` provides the analogous backward summary-based engine for clients such as liveness.
- **Linear strategy** (Newton only): `LinearStrategy::SCC`, `AdaptiveScc`, or `TensorProduct` (when LCFL).
- **Subdistributive summaries**: constant propagation and interval analysis use `SummaryTransformerDomain`, a first-class abstract-summary path for the current Section 8-style in-tree clients.
- **Developer note**: JACM core Newton lives in `Core/Solver.h`, `Core/Diff.h`, and the linear solvers; TOPLAS tensor regularization lives in `Core/Tensor*`; the Section 8-style in-tree clients are the bounded abstract-summary CP/interval analyses, which are deliberately approximate rather than exact restatements of the paper.
- **Legacy explicit summaries**: `ProgramTransferDomain` remains available for tests and older utility clients that want explicit path sets.
- **Solver status**: direct solver APIs return `Stat`. `converged` now means “stabilized without approximation hooks”; `used_approx_equal`, `hit_outer_limit`, `hit_linear_limit`, and `hit_fixpoint_limit` record the approximation sources explicitly. Under `AdaptiveScc`, `adaptive_scc_direct_count`, `adaptive_scc_worklist_count`, `adaptive_scc_tensor_count`, and `adaptive_scc_tensor_fallback_count` aggregate the SCC-local choices made across all adaptive linear solves performed during the Newton run. Interprocedural engines/results expose `AnalysisStatus`, including `used_summary_overflow`, `used_fact_widening`, and `used_bounded_inner_solve`.

## Domain hooks (optional)

- `static constexpr bool commutative_extend`: If declared and true, `NewtonSolver` will cap the default outer iteration bound to `n` (number of equations), matching the JACM termination bound for idempotent commutative semirings.
- If that automatic `n`-step bound does not actually converge, the solver now falls back to an uncapped run instead of silently returning a truncated result.
- `static bool approx_equal(value_type a, value_type b)`: If declared, used instead of `equal()` for convergence/stability checks. Results that stabilize under this hook are reported as approximate rather than exact.
- `static constexpr bool project_newton_safe`: If declared and true, the domain opts into using `Project` on Newton/tensor paths. Domains that merely implement `project()` remain evaluable in plain fixpoint evaluation but are rejected on Newton/tensor paths.
- `static value_type choose_delta(value_type f_nu, value_type nu)`: If declared, used to pick the Newton δ(i) term for **non-idempotent** domains (instead of requiring `subtract`). The solver validates the required invariant `combine(nu, delta) == f_nu` using exact domain equality (`equal`, not `approx_equal`) and throws `InvalidNewtonDeltaError` if the domain violates it.
- `static constexpr int max_fixpoint_iters`: If declared (≥0), caps generic fixpoint iteration (used by `Star`, `Mu`, and the naive linear solver). This intentionally turns the result into a bounded approximation; `Stat::hit_fixpoint_limit` records the source.
- `static constexpr long max_linear_steps`: If declared (≥0), caps linear-solver steps when solving the linearized system (useful for numeric semirings). This also yields a bounded approximation rather than the paper's least solution; `Stat::hit_linear_limit` records the source.
- Domains that opt into `project_newton_safe` are responsible for the proof obligations used by the Newton path: monotonicity, compatibility with `combine`, compatibility with the summary/linearized equations, and any tensor-side readout restrictions required by the domain.

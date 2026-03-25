Newtonian Program Analysis (NPA)
================================

Overview
========

The **Newtonian Program Analysis (NPA)** engine in ``lib/Dataflow/NPA``
implements advanced, research-oriented techniques for compositional and
recurrence-based data-flow reasoning.
It is designed for **numeric** and **relational** analyses that go
beyond classical bit-vector or IFDS/IDE formulations.

* **Location**: ``lib/Dataflow/NPA`` (implementation), ``include/Dataflow/NPA`` (headers)
* **Purpose**: library infrastructure for Newton-style program analyses

Conceptual Background
=====================

NPA is based on a sequence of works that recast program analysis as a
form of **Newton iteration** over suitable abstract domains:

* *Newtonian Program Analysis* (JACM 2010) [EsparzaKieferLuttenberger2010]_:
  algebraic differential :math:`Df|_\nu`, Newton sequence, convergence to least fixed point.
* *Newtonian Program Analysis via Tensor Product* (TOPLAS 2016) [RepsEtAlTOPLAS2016]_:
  solving the **LCFL** (linear context-free language) sub-problems produced each
  Newton round by "regularizing" them via a **tensor product** (paired semiring).
* *Compositional Recurrence Analysis Revisited*, PLDI 2017.

The foundational paper [EsparzaKieferLuttenberger2010]_ (JACM) presents a novel
generic technique for solving interprocedural dataflow equations by generalizing
Newton's method to **ω-continuous semirings**. When the semiring multiplication
is **non-commutative** (e.g. function composition in dataflow), the linearized
system at each Newton round is an **LCFL equation system** (coefficients on both
sides of variables). [RepsEtAlTOPLAS2016]_ shows
how to convert such systems into left-linear (regular) form over a paired semiring,
then solve and project back (Algorithm 3.4).

Key Insight: Algebraic Generalization
--------------------------------------

The key insight is that Newton's method can be reformulated **purely algebraically**,
without requiring subtraction, division, or limits. This allows the method to be
applied to arbitrary semirings, not just the real numbers.

The method works as follows:

* Programs are encoded as systems of (possibly non-linear) recurrence equations
  over an ω-continuous semiring.
* At each iteration, Newton's method linearizes the system at the current
  approximation using **algebraic differentials**.
* The differential of a power series is defined inductively using algebraic rules
  (product rule, sum rule) rather than limits.
* Each Newton step solves a linear system to obtain the next approximation.

This yields:

* **Faster convergence** than classical Kleene iteration (often exponentially faster).
* **Robustness**: guaranteed convergence for any ω-continuous semiring.
* **Termination guarantees**: for idempotent commutative semirings, Newton's method
  terminates in at most *n* iterations for a system of *n* equations. In code, this
  bound is applied automatically only when the domain declares
  ``static constexpr bool commutative_extend = true``.

Mathematical Foundation
========================

ω-Continuous Semirings
----------------------

An **ω-continuous semiring** is a tuple :math:`\langle S, +, \cdot, 0, 1 \rangle` where:

* :math:`(S, +, 0)` is a commutative monoid (addition/join operation)
* :math:`(S, \cdot, 1)` is a monoid (multiplication/sequencing operation)
* Multiplication distributes over addition
* The relation :math:`a \sqsubseteq b` (defined as :math:`\exists d : a + d = b`) is a partial order
* Every ω-chain has a supremum with respect to :math:`\sqsubseteq`
* Infinite sums satisfy standard continuity properties

This generalizes:

* **Lattices** (classical dataflow analysis): idempotent semirings where :math:`a + a = a`
* **Language semirings**: languages over an alphabet with union and concatenation
* **Counting semirings**: sets of vectors (Parikh images) for counting occurrences
* **Probabilistic semirings**: nonnegative reals for probability and expected runtime analysis
* **Relational semirings**: relations over program states for may-alias and summary relations

Comparison with Kleene's Method
--------------------------------

Traditional dataflow analysis uses **Kleene iteration**, which:

* Starts at :math:`\kappa^{(0)} = 0` (or :math:`f(0)`)
* Iterates: :math:`\kappa^{(i+1)} = f(\kappa^{(i)})`
* Converges to the least fixed point :math:`\mu f = \sup_i \kappa^{(i)}`

**Newton's method** instead:

* Starts at :math:`\nu^{(0)} = f(0)`
* Iterates: :math:`\nu^{(i+1)} = \nu^{(i)} + \Delta^{(i)}` where :math:`\Delta^{(i)}` is the
  least solution of the linearized system :math:`Df|_{\nu^{(i)}}(X) + \delta^{(i)} = X`
* Converges at least as fast as Kleene: :math:`\kappa^{(i)} \sqsubseteq \nu^{(i)} \sqsubseteq \mu f`

For probabilistic programs, Newton's method can achieve linear convergence (one bit
of precision per iteration) compared to logarithmic convergence for Kleene iteration.
For commutative idempotent semirings, Newton's method guarantees termination in at
most *n* iterations for *n* equations.

Examples and Applications
=========================

The NPA framework supports both **qualitative** and **quantitative** analyses:

May-Alias Analysis
------------------

Using the **counting semiring** (Parikh abstraction), NPA can perform may-alias
analysis that tracks how many times each data access path is traversed. Unlike
Kleene iteration, which may not terminate for recursive procedures, Newton's method
terminates in one iteration for this class of problems.

Average Runtime Analysis
------------------------

Using **probabilistic semirings**, NPA can compute:
* The probability that a procedure terminates
* The expected runtime (conditional on termination)

For probabilistic programs with recursive procedures, Newton's method converges
substantially faster than Kleene iteration, achieving linear rather than logarithmic
convergence.

LCFL Sub-Problems and Tensor-Product Regularization
--------------------------------------------------

When the semiring extend (multiplication) is **non-commutative**, the linearized
system at each Newton round has the form of an **LCFL equation system**
:math:`Y_j = c_j \oplus \bigoplus_{i,k} (a_{i,j,k} \otimes Y_i \otimes b_{i,j,k})`
(Reps et al. TOPLAS 2016, Definition 3.1): coefficients appear on **both sides**
of variables, corresponding to path problems described by linear context-free
languages. Conventional intraprocedural (regular) solvers cannot be applied
directly.

The **tensor-product** approach (Algorithm 3.4 in [RepsEtAlTOPLAS2016]_):
(1) Convert the LCFL system into a **left-linear** system over a **paired**
semiring :math:`(a,b) \otimes_p (a',b') = (a' \otimes a, b \otimes b')`,
:math:`R((w_1,w_2)) = w_1 \otimes w_2`; (2) solve the left-linear system
(e.g. by worklist or path expressions); (3) **project** back via :math:`R`.
The paired structure maintains the mirrored correlation of left/right
coefficients. The implementation uses ``TensorProductDomain`` and
``solve_linear_tensor_impl`` when the system has LCFL structure (Concat/Star).

``Star`` is the paper-faithful Kleene-star construct used by Newton/tensor
regularization. ``Mu`` remains available as a generic least-fixpoint construct
for plain evaluation, but it is intentionally rejected on Newton/tensor paths.

Implementation note: to preserve left/right correlation during projection, the
tensor solver uses an exact correlated representation for idempotent domains
(modeling sums as finite sets of pairs) rather than componentwise addition over a
single pair.

Other Applications
------------------

* **Language analysis**: Computing the set of execution paths (context-free languages)
* **Relational analysis**: Computing summary relations for interprocedural analysis
* **Cost analysis**: Expected resource consumption under probabilistic execution models

Distributive vs. Non-Distributive Analyses
===========================================

NPA supports both **distributive** and **non-distributive** program analyses:

* **Distributive analyses**: Multiplication distributes over addition
  (:math:`a \cdot (b + c) = a \cdot b + a \cdot c`). In this case, the least fixed point
  coincides with the join-over-all-paths (JOP) solution.

* **Non-distributive analyses**: Only subdistributivity holds
  (:math:`a \cdot (b + c) \sqsupseteq a \cdot b + a \cdot c`). In this case, the least
  fixed point is an **overapproximation** of the JOP solution, but still provides a
  sound analysis result. In-tree clients such as constant propagation and interval
  analysis use ``SummaryTransformerDomain`` as NPA's current abstract-summary path
  for this fragment.

Core Implementation (include/Dataflow/NPA/Core)
===============================================

The core headers implement the algorithms from Esparza et al. (JACM) and Reps et al. (TOPLAS 2016):

* **NPACommon.h**: Domain concept (ω-continuous semiring), ``LinearStrategy``
  (Naive, Worklist, SCC, TensorProduct).
* **Expressions.h**: ``Exp0`` (polynomial equation AST), ``Exp1`` (linearized AST);
  ``Concat`` encodes the LCFL form :math:`a \cdot X \cdot b`; ``Star`` is the
  Newton/tensor Kleene-star fragment and ``Mu`` is a generic least-fixpoint node.
* **Diff.h**: Builds the differential :math:`Df|_\nu` from a polynomial expression
  (Esparza et al. JACM, Defn. 3.1, 3.5) and caches differential shape plans across
  Newton rounds.
* **Eval.h**: ``I0`` evaluates Exp0 (full system); ``I1`` evaluates Exp1 (linear RHS).
* **Fixpoint.h**: ``fix`` / ``fix_vec`` for Kleene-like iteration.
* **LCFLDetector.h**: Detects Concat/Star (LCFL structure).
* **LinearSolvers.h**: ``solve_linear_worklist_impl``, ``solve_linear_scc_impl``,
  ``solve_linear_tensor_impl`` for the linearized system.
* **TensorLinearSolve.h**: Tensor-product solver (Reps et al. Alg. 3.4).
* **Solver.h**: ``KleeneIter`` (κ^(i+1) = f(κ^(i))), ``NewtonIter`` (ν^(i+1) = ν^(i) ⊔ Δ^(i)).

Usage Notes
===========

In the current code base, NPA is primarily exposed as an internal
library component.
Clients that use NPA are expected to be familiar with the above papers
and to instantiate the provided abstractions for their specific numeric
domains and recurrence schemes.

Interprocedural clients currently assume a closed-world LLVM module for indirect
calls: if no stronger resolver is attached, unknown call sites are approximated
by the set of type-compatible defined functions in the current module.

Practical notes for numeric domains
===================================

For floating-point / numeric semirings, exact equality often prevents termination
in iterative solvers. Domains may provide an optional method
``approx_equal(a,b)``; when present, NPA uses it for convergence checks instead
of ``equal(a,b)``.

Domains may also opt into bounding iteration:

* ``max_fixpoint_iters`` caps generic fixpoint loops (e.g. ``Star`` / ``Mu`` bodies).
* ``max_linear_steps`` caps worklist/SCC steps for the linearized system.

If a domain implements ``project()``, Newton/tensor paths require an additional
opt-in ``project_newton_safe`` contract. That contract is the domain author's
assertion that projection is monotone and compatible with ``combine`` and the
linearized summary equations used by the Newton pipeline.

This engine is **not** currently wired into a dedicated command-line
tool; instead, it serves as a building block for experimental analyses
within Lotus.

References
==========

.. [RepsEtAlTOPLAS2016] Thomas Reps, Emma Turetsky, and Prathmesh Prabhu.
   Newtonian Program Analysis via Tensor Product. ACM Transactions on Programming
   Languages and Systems (TOPLAS), 2016. (Portions in POPL 2016.)

.. [EsparzaKieferLuttenberger2010] Javier Esparza, Stefan Kiefer, and Michael Luttenberger.
   Newtonian Program Analysis. Journal of the ACM, 57(6):1-47, 2010.

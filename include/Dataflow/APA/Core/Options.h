#ifndef DATAFLOW_ELIMINATION_CORE_OPTIONS_H_
#define DATAFLOW_ELIMINATION_CORE_OPTIONS_H_

#include <cstddef>

namespace elimination {

enum class EliminationMethod {
  // Generic O(n^3) state-elimination (Floyd–Warshall-style) over all nodes.
  StateElimination,
  // Paper-style ADT "simple" algorithm path-expression updates.
  ADTSimple,
  // Paper-style ADT + path-expression construction (requires reducible info).
  ADTDelayed,
};

enum class OnNonConvergentStar {
  // Abort solve with NonConvergentStar status.
  Fail,
  // Return the last iterand at the iteration bound.
  ReturnLast,
  // Return meet identity at the iteration bound.
  ReturnIdentity,
};

enum class SolveStatus {
  Ok,
  FallbackToState,
  NonConvergentStar,
  InvalidProblem,
};

enum class FallbackReason {
  None,
  ADTRejected,
  InvalidProblem,
};

struct SolveDiagnostics final {
  bool used_adt = false;
  EliminationMethod requested_method = EliminationMethod::StateElimination;
  EliminationMethod executed_method = EliminationMethod::StateElimination;
  FallbackReason fallback_reason = FallbackReason::None;
  std::size_t star_iterations_total = 0;
  bool max_star_hit = false;
};

struct EliminationOptions final {
  EliminationMethod Method = EliminationMethod::StateElimination;
  OnNonConvergentStar NonConvergentStarPolicy = OnNonConvergentStar::Fail;
  // 0 means "use Problem.maxStarIterations()".
  std::size_t MaxStarIterations = 0;
  // Reserved for future conditional collection. Diagnostics are currently
  // recorded unconditionally by the solver and attached to result metadata.
  bool RecordDiagnostics = true;
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_CORE_OPTIONS_H_

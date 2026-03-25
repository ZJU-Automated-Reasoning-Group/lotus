#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"

namespace tpa {

void TransferFunction::evalEntryNode(const ProgramPoint &pp,
                                     EvalResult &evalResult) {
  assert(localState != nullptr);

  addTopLevelSuccessors(pp, evalResult);
  addMemLevelSuccessors(pp, *localState, evalResult);

  // To prevent the analysis from converging before a newly added return edge
  // is processed, we need to force the analysis to check the return targets
  // whenever a function is entered.
  //
  // Note: this unconditional enqueue is intentional. The SemiSparsePropagator
  // uses a memoization table (Memo) and will only actually re-process the exit
  // node if the store state at that point has changed. So while this enqueues
  // the exit node on every entry visit, the propagator suppresses redundant
  // work via memo-table checks, keeping the overall fixpoint correct and
  // terminating.
  auto &cfg = pp.getCFGNode()->getCFG();
  if (!cfg.doesNotReturn())
    evalResult.addTopLevelProgramPoint(
        ProgramPoint(pp.getContext(), cfg.getExitNode()));
}

} // namespace tpa

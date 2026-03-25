#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

namespace tpa {

void TransferFunction::evalCopyNode(const ProgramPoint &pp,
                                    EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &copyNode = static_cast<const CopyCFGNode &>(*pp.getCFGNode());

  std::vector<PtsSet> srcPtsSets;
  srcPtsSets.reserve(copyNode.getNumSrc());

  auto &ptrManager = globalState.getPointerManager();
  auto &env = globalState.getEnv();
  for (const auto *src : copyNode) {
    const auto *srcPtr = ptrManager.getPointer(ctx, src);

    // This must happen in a PHI node, where one operand must be defined after
    // the CopyNode itself. We need to proceed because the operand may depend on
    // the rhs of this CopyNode and if we give up here, the analysis will reach
    // an immature fixpoint.
    if (srcPtr == nullptr)
      continue;

    auto pSet = env.lookup(srcPtr);
    // Bug fix (same class as CallTransfer Bug 7): previously an empty
    // points-to set caused an early return, which could cause a premature
    // fixpoint if the operand's set is computed later in the iteration.
    // Now we include the empty set as a placeholder and continue; the
    // merge will simply contribute nothing for this operand, and the node
    // will be re-evaluated when the operand's set changes.
    srcPtsSets.emplace_back(pSet);
  }

  const auto *dstPtr = ptrManager.getOrCreatePointer(ctx, copyNode.getDest());
  auto dstSet = PtsSet::mergeAll(srcPtsSets);
  auto envChanged = globalState.getEnv().strongUpdate(dstPtr, dstSet);

  if (envChanged)
    addTopLevelSuccessors(pp, evalResult);
}

} // namespace tpa

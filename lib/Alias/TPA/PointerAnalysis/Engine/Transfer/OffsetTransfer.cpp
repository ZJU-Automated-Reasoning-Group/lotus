#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"

namespace tpa {

PtsSet TransferFunction::offsetMemory(const MemoryObject *srcObj, size_t offset,
                                      bool isArrayRef) {
  assert(srcObj != nullptr);

  auto resSet = PtsSet::getEmptySet();
  auto &memManager = globalState.getMemoryManager();

  // We have two cases here:
  // - For non-array reference, just access the variable with the given offset
  // - For array reference, we have to examine the variables with offset * 0,
  // offset * 1, offset * 2... all the way till the memory region boundary, if
  // the memory object is not known to be an array previously (this may happen
  // if the program contains nonarray-to-array bitcast)
  if (isArrayRef && offset != 0) {
    auto objSize = srcObj->getMemoryBlock()->getTypeLayout()->getSize();
    // errs() << "obj = " << *srcObj << ", size = " << objSize -
    // srcObj->getOffset() << "\n";

    for (unsigned i = 0, e = objSize - srcObj->getOffset(); i < e;
         i += offset) {
      const auto *offsetObj = memManager.offsetMemory(srcObj, i);
      resSet = resSet.insert(offsetObj);
    }
  } else {
    const auto *offsetObj = memManager.offsetMemory(srcObj, offset);
    resSet = resSet.insert(offsetObj);
  }
  return resSet;
}

bool TransferFunction::copyWithOffset(const Pointer *dst, const Pointer *src,
                                      size_t offset, bool isArrayRef) {
  assert(dst != nullptr && src != nullptr);

  auto &env = globalState.getEnv();
  auto srcSet = env.lookup(src);
  if (srcSet.empty())
    return false;

  std::vector<PtsSet> srcPtsSets;
  srcPtsSets.reserve(srcSet.size());

  for (const auto *srcObj : srcSet) {
    // Universal object: GEP on an unknown pointer yields an unknown pointer.
    if (srcObj->isUniversalObject()) {
      srcPtsSets.emplace_back(
          PtsSet::getSingletonSet(MemoryManager::getUniversalObject()));
      break;
    }

    // Null object: GEP on null is undefined behaviour; skip it.
    // Previously this was silently skipped and then promoted to Universal when
    // the result set was empty, which introduced spurious aliasing. Now we
    // simply skip null objects and let the result remain empty (or be merged
    // with other non-null contributions). A future improvement could report a
    // potential null-pointer dereference here.
    if (srcObj->isNullObject())
      continue;

    auto pSet = offsetMemory(srcObj, offset, isArrayRef);
    srcPtsSets.emplace_back(pSet);
  }

  PtsSet resSet = PtsSet::mergeAll(srcPtsSets);

  // Bug fix: do NOT promote an empty result to Universal. An empty result means
  // the source set contained only null objects (GEP on null is UB) or the
  // offset was out of bounds. Returning Universal here caused spurious aliasing
  // throughout the analysis. Instead, return false (no environment change) so
  // the worklist does not propagate stale information.
  if (resSet.empty())
    return false;

  return env.strongUpdate(dst, resSet);
}

void TransferFunction::evalOffsetNode(const ProgramPoint &pp,
                                      EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &offsetNode = static_cast<const OffsetCFGNode &>(*pp.getCFGNode());
  auto &ptrManager = globalState.getPointerManager();

  const auto *srcPtr = ptrManager.getPointer(ctx, offsetNode.getSrc());
  if (srcPtr == nullptr)
    return;
  const auto *dstPtr = ptrManager.getOrCreatePointer(ctx, offsetNode.getDest());

  auto envChanged = copyWithOffset(dstPtr, srcPtr, offsetNode.getOffset(),
                                   offsetNode.isArrayRef());

  if (envChanged)
    addTopLevelSuccessors(pp, evalResult);
}

} // namespace tpa

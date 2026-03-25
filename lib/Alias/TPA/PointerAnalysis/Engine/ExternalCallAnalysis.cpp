// Implementation of External Call Analysis.
//
// This file handles calls to external functions (functions without definitions
// in the module). Instead of analyzing the body of the function, it relies on
// an "External Pointer Table" (annotations) to model the side effects of the
// call on pointers and memory.
//
// Supported Effects:
// - Alloc: The function allocates memory (like malloc).
// - Copy: The function copies data between pointers (like memcpy, strcpy) or
// assigns values.
// - Exit: The function terminates the program (like exit).
//
// If no annotation is found, the analysis conservatively assumes the function
// acts as a no-op regarding the tracked state (unless it returns a value, which
// is effectively lost/universal). NOTE: A robust analysis should probably
// assume "unknown behavior" escapes everything, but this implementation seems
// to rely on explicit annotations for correctness.

#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Type/TypeLayout.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"
#include "Annotation/Pointer/ExternalPointerTable.h"
#include "Annotation/Pointer/PointerEffect.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/raw_ostream.h>

using namespace annotation;
using namespace llvm;

namespace tpa {

// Helper to extract the LLVM Value corresponding to a position argument (e.g.,
// Arg(0), Ret).
static const Value *getArgument(const CallCFGNode &callNode,
                                const APosition &pos) {
  const auto *inst = callNode.getCallSite();
  if (pos.isReturnPosition())
    return inst;

  // We can't just call callNode.getArgument(...) because there might be
  // non-pointer args that are not included in callNode
  const auto *callBase = dyn_cast<CallBase>(inst);
  assert(callBase);

  auto argIdx = pos.getAsArgPosition().getArgIndex();
  assert(callBase->arg_size() > argIdx);

  return callBase->getArgOperand(argIdx)->stripPointerCasts();
}

// Heuristic to determine the type of memory allocated by a malloc-like call.
// Looks at the uses of the call instruction (specifically BitCasts) to infer
// the intended type.
//
// Bug fix (2i): the previous implementation only examined *direct* users of
// the call instruction. A common pattern after optimization passes is:
//
//   %p = call i8* @malloc(...)
//   store i8* %p, i8** %slot          ; direct store — no bitcast visible
//   %q = load SomeType** %slot        ; cast happens at the load site
//   %r = bitcast i8* %p to SomeType*  ; or: bitcast is in a different BB
//
// In such cases the direct-user scan found no BitCastInst and fell back to
// byte-array layout, losing type precision.
//
// Improvement: after scanning direct users, if no unique BitCast type was
// found, also scan users of any StoreInst that stores the malloc result, and
// look at the corresponding load sites for BitCasts. This covers the most
// common indirect pattern without a full def-use traversal.
static Type *getMallocType(const Instruction *callInst) {
  assert(callInst != nullptr);

  // Helper: given a pointer value, collect all BitCast destination types seen
  // among its direct users.
  auto collectBitCastTypes = [](const Value *val, size_t &numBitCasts,
                                size_t &numOther, PointerType *&lastBCType) {
    for (const auto *user : val->users()) {
      if (const auto *bcInst = dyn_cast<BitCastInst>(user)) {
        lastBCType = cast<PointerType>(bcInst->getDestTy());
        numBitCasts++;
      } else if (isa<GetElementPtrInst>(user)) {
        numOther++;
      }
    }
  };

  PointerType *mallocType = nullptr;
  size_t numOfBitCastUses = 0;
  size_t numOfOtherUses = 0;

  // Phase 1: scan direct users of the call instruction.
  collectBitCastTypes(callInst, numOfBitCastUses, numOfOtherUses, mallocType);

  // Phase 2: if no unique BitCast found yet, look through store→load chains.
  // This handles the pattern where the malloc result is stored to a local slot
  // and then loaded (possibly with a different type) elsewhere.
  if (numOfBitCastUses == 0) {
    for (const auto *user : callInst->users()) {
      if (const auto *stInst = dyn_cast<StoreInst>(user)) {
        // The malloc result is stored into a memory slot. Look at all loads
        // from that slot and check their users for BitCasts.
        const auto *slot = stInst->getPointerOperand();
        for (const auto *slotUser : slot->users()) {
          if (const auto *ldInst = dyn_cast<LoadInst>(slotUser)) {
            collectBitCastTypes(ldInst, numOfBitCastUses, numOfOtherUses,
                                mallocType);
          }
        }
      }
    }
  }

  // Exactly one BitCast use and no other ambiguous uses: use the cast's
  // destination element type as the allocation type.
  if (numOfBitCastUses == 1 && mallocType != nullptr)
    return mallocType->getNonOpaquePointerElementType();

  // No BitCast and no GEP uses: the malloc result is used directly as its
  // declared return type (e.g., already the right pointer type).
  if (numOfBitCastUses == 0 && numOfOtherUses == 0)
    return callInst->getType()->getNonOpaquePointerElementType();

  // Multiple or ambiguous uses: type cannot be determined reliably.
  // Return nullptr to fall back to byte-array layout.
  return nullptr;
}

// Checks if the allocation size matches the size of a single instance of the
// type. If not, it's likely an array allocation.
static bool isSingleAlloc(const TypeLayout *typeLayout,
                          const llvm::Value *sizeVal) {
  if (sizeVal == nullptr)
    return false;

  if (const auto *cInt = dyn_cast<ConstantInt>(sizeVal)) {
    auto size = cInt->getZExtValue();
    auto typeSize = typeLayout->getSize();
    // Bug fix: replaced assert with a graceful check. If the allocation size is
    // not a multiple of the type size (e.g., due to alignment padding or
    // programmer computing sizeof(T)+extra), the assert would crash the
    // analysis. Instead, treat such cases as array/unknown allocations by
    // returning false, which causes the caller to fall back to byte-array
    // layout.
    if (typeSize == 0 || size % typeSize != 0)
      return false;
    return size == typeSize;
  }

  return false;
}

// Logic for handling malloc-like allocations with a size argument.
bool TransferFunction::evalMallocWithSize(const context::Context *ctx,
                                          const llvm::Instruction *dstVal,
                                          llvm::Type *mallocType,
                                          const llvm::Value *mallocSize) {
  assert(ctx != nullptr && dstVal != nullptr);

  const TypeLayout *typeLayout = nullptr;
  if (mallocType == nullptr)
    typeLayout = TypeLayout::getByteArrayTypeLayout();
  else {
    typeLayout =
        globalState.getSemiSparseProgram().getTypeMap().lookup(mallocType);
    assert(typeLayout != nullptr);
    // If we can't confirm it's a single object allocation, treat as array
    if (!isSingleAlloc(typeLayout, mallocSize))
      // TODO: adjust type layout when mallocSize is known (e.g. array count)
      typeLayout = TypeLayout::getByteArrayTypeLayout();
  }

  return evalMemoryAllocation(ctx, dstVal, typeLayout, true);
}

// Logic for Alloc effects.
bool TransferFunction::evalExternalAlloc(
    const context::Context *ctx, const CallCFGNode &callNode,
    const PointerAllocEffect &allocEffect) {
  // TODO: add type hint to malloc-like calls
  const auto *dstVal = callNode.getDest();
  if (dstVal == nullptr)
    return false;

  auto *mallocType = getMallocType(callNode.getCallSite());
  const auto *sizeVal =
      allocEffect.hasSizePosition()
          ? getArgument(callNode, allocEffect.getSizePosition())
          : nullptr;

  return evalMallocWithSize(ctx, dstVal, mallocType, sizeVal);
}

// Helper to simulate memcpy semantics on the store.
// Copies values from srcObjs to dstObj (and reachable objects).
void TransferFunction::evalMemcpyPtsSet(
    const MemoryObject *dstObj,
    const std::vector<const MemoryObject *> &srcObjs, size_t startingOffset,
    Store &store) {
  auto &memManager = globalState.getMemoryManager();
  for (const auto *srcObj : srcObjs) {
    auto srcSet = store.lookup(srcObj);
    if (srcSet.empty())
      continue;

    // Calculate relative offset and find target sub-object
    auto offset = srcObj->getOffset() - startingOffset;
    const auto *tgtObj = memManager.offsetMemory(dstObj, offset);
    if (tgtObj->isSpecialObject())
      break;

    // Copy the points-to set (weak update because we are merging)
    store.weakUpdate(tgtObj, srcSet);
  }
}

// Resolves pointers for memcpy and iterates over source/dest objects.
bool TransferFunction::evalMemcpyPointer(const Pointer *dst, const Pointer *src,
                                         Store &store) {
  auto &env = globalState.getEnv();

  auto dstSet = env.lookup(dst);
  if (dstSet.empty())
    return false;
  auto srcSet = env.lookup(src);
  if (srcSet.empty())
    return false;

  auto &memManager = globalState.getMemoryManager();
  for (const auto *srcObj : srcSet) {
    // Get all objects reachable from the source pointer's object (deep copy?)
    // Actually, getReachablePointerObjects usually returns the object +
    // sub-objects (fields/array elements).
    auto srcObjs = memManager.getReachablePointerObjects(srcObj);
    for (const auto *dstObj : dstSet)
      evalMemcpyPtsSet(dstObj, srcObjs, srcObj->getOffset(), store);
  }
  return true;
}

// Entry point for memcpy effect.
bool TransferFunction::evalMemcpy(const context::Context *ctx,
                                  const CallCFGNode &callNode, Store &store,
                                  const APosition &dstPos,
                                  const APosition &srcPos) {
  assert(dstPos.isArgPosition() && srcPos.isArgPosition() &&
         "memcpy only operates on arguments");

  auto &ptrManager = globalState.getPointerManager();
  const auto *dstPtr =
      ptrManager.getPointer(ctx, getArgument(callNode, dstPos));
  if (dstPtr == nullptr)
    return false;
  const auto *srcPtr =
      ptrManager.getPointer(ctx, getArgument(callNode, srcPos));
  if (srcPtr == nullptr)
    return false;

  return evalMemcpyPointer(dstPtr, srcPtr, store);
}

// Determines the points-to set for a Copy source.
PtsSet TransferFunction::evalExternalCopySource(const context::Context *ctx,
                                                const CallCFGNode &callNode,
                                                const CopySource &src) {
  switch (src.getType()) {
  case CopySource::SourceType::Value: {
    // Source is the value of the pointer argument itself (e.g., p = q)
    const auto *ptr = globalState.getPointerManager().getPointer(
        ctx, getArgument(callNode, src.getPosition()));
    if (ptr == nullptr)
      return PtsSet::getEmptySet();
    return globalState.getEnv().lookup(ptr);
  }
  case CopySource::SourceType::DirectMemory: {
    // Source is the content of memory pointed to by argument (e.g., p = *q)
    const auto *ptr = globalState.getPointerManager().getPointer(
        ctx, getArgument(callNode, src.getPosition()));
    if (ptr == nullptr)
      return PtsSet::getEmptySet();
    return loadFromPointer(ptr, *localState);
  }
  case CopySource::SourceType::Universal: {
    return PtsSet::getSingletonSet(MemoryManager::getUniversalObject());
  }
  case CopySource::SourceType::Null: {
    return PtsSet::getSingletonSet(MemoryManager::getNullObject());
  }
  case CopySource::SourceType::Static:
    // TODO: model "static" memory
    return PtsSet::getSingletonSet(MemoryManager::getUniversalObject());
  case CopySource::SourceType::ReachableMemory: {
    llvm_unreachable("ReachableMemory src should be handled earlier");
  }
  }
}

// Helper to fill a destination pointer's reachable memory with a source set.
// This is used when a function copies a value into *all* reachable sub-fields
// of a struct/array.
void TransferFunction::fillPtsSetWith(const Pointer *ptr, PtsSet srcSet,
                                      Store &store) {
  auto pSet = globalState.getEnv().lookup(ptr);

  for (const auto *obj : pSet) {
    if (obj->isSpecialObject())
      continue;

    auto candidateObjs =
        globalState.getMemoryManager().getReachablePointerObjects(obj);
    for (const auto *tgtObj : candidateObjs)
      store.weakUpdate(tgtObj, srcSet);
  }
}

// Applies the copy result to the destination.
void TransferFunction::evalExternalCopyDest(const context::Context *ctx,
                                            const CallCFGNode &callNode,
                                            EvalResult &evalResult,
                                            const CopyDest &dest,
                                            PtsSet srcSet) {
  // If the return value is not used, don't bother process it
  bool envChanged = false;
  if (!(callNode.getDest() == nullptr &&
        dest.getPosition().isReturnPosition())) {
    const auto *dstPtr = globalState.getPointerManager().getOrCreatePointer(
        ctx, getArgument(callNode, dest.getPosition()));
    switch (dest.getType()) {
    case CopyDest::DestType::Value: {
      // Destination is a pointer variable (e.g., p = ...).
      // Bug fix: always propagate mem-level successors regardless of whether
      // the env changed. Previously, if envChanged was false (no new pts-to
      // info for the destination), neither top-level nor mem-level successors
      // were added for this branch, silently dropping the call node's
      // successors. The store is unchanged here, so we pass *localState.
      envChanged = globalState.getEnv().weakUpdate(dstPtr, srcSet);
      addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                            evalResult);
      break;
    }
    case CopyDest::DestType::DirectMemory: {
      // Destination is memory pointed to by argument (e.g., *p = ...)
      auto dstSet = globalState.getEnv().lookup(dstPtr);
      if (dstSet.empty()) {
        // Destination not yet resolved; still propagate the unmodified store
        // so mem-level successors are not silently dropped.
        addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                              evalResult);
        return;
      }

      auto &store = evalResult.getNewStore(*localState);
      weakUpdateStore(dstSet, srcSet, store);
      addMemLevelSuccessors(ProgramPoint(ctx, &callNode), store, evalResult);
      break;
    }
    case CopyDest::DestType::ReachableMemory: {
      // Destination is all memory reachable from argument
      auto &store = evalResult.getNewStore(*localState);
      fillPtsSetWith(dstPtr, srcSet, store);
      addMemLevelSuccessors(ProgramPoint(ctx, &callNode), store, evalResult);
      break;
    }
    }
  }

  if (envChanged)
    addTopLevelSuccessors(ProgramPoint(ctx, &callNode), evalResult);
}

// Dispatches copy effects (assignment, load, store, memcpy).
void TransferFunction::evalExternalCopy(const context::Context *ctx,
                                        const CallCFGNode &callNode,
                                        EvalResult &evalResult,
                                        const PointerCopyEffect &copyEffect) {
  auto const &src = copyEffect.getSource();
  auto const &dest = copyEffect.getDest();

  // Special case for memcpy: the source is not a single ptr/mem
  if (src.getType() == CopySource::SourceType::ReachableMemory) {
    assert(dest.getType() == CopyDest::DestType::ReachableMemory &&
           "R src can only be assigned to R dest");

    auto &store = evalResult.getNewStore(*localState);
    auto storeChanged =
        evalMemcpy(ctx, callNode, store, dest.getPosition(), src.getPosition());

    // Fix #4: Always propagate mem-level successors regardless of whether
    // evalMemcpy reported a store change. If storeChanged is false (e.g.,
    // because the source/dest sets are not yet resolved), we still need to
    // propagate the current store so that the call node's successors are not
    // silently dropped. When the src/dst sets are resolved later, the call
    // node will be re-evaluated and the store will be updated then.
    addMemLevelSuccessors(ProgramPoint(ctx, &callNode),
                          storeChanged ? store : *localState, evalResult);
  } else {
    // General case: src is Value/DirectMemory/etc.
    auto srcSet = evalExternalCopySource(ctx, callNode, src);
    if (!srcSet.empty())
      evalExternalCopyDest(ctx, callNode, evalResult, dest, srcSet);
  }
}

// Dispatches based on effect type (Alloc, Copy, Exit).
void TransferFunction::evalExternalCallByEffect(const context::Context *ctx,
                                                const CallCFGNode &callNode,
                                                const PointerEffect &effect,
                                                EvalResult &evalResult) {
  switch (effect.getType()) {
  case PointerEffectType::Alloc: {
    if (evalExternalAlloc(ctx, callNode, effect.getAsAllocEffect()))
      addTopLevelSuccessors(ProgramPoint(ctx, &callNode), evalResult);
    addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                          evalResult);
    break;
  }
  case PointerEffectType::Copy: {
    evalExternalCopy(ctx, callNode, evalResult, effect.getAsCopyEffect());
    break;
  }
  case PointerEffectType::Exit:
    // Exit effect: do not add any successors, terminating the path.
    break;
  }
}

// Main handler for external calls.
// Checks intrinsic ID or looks up the ExternalPointerTable.
void TransferFunction::evalExternalCall(const context::Context *ctx,
                                        const CallCFGNode &callNode,
                                        const FunctionContext &fc,
                                        EvalResult &evalResult) {
  // Handle LLVM intrinsics that are relevant/ignorable
  if (fc.getFunction()->isIntrinsic()) {
    switch (fc.getFunction()->getIntrinsicID()) {
    case Intrinsic::dbg_value:
    case Intrinsic::dbg_declare:
    case Intrinsic::dbg_label:
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
      // These are no-ops for pointer analysis
      addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                            evalResult);
      return;
    default:
      break;
    }
  }

  // Look up annotations for library functions
  const auto *summary =
      globalState.getExternalPointerTable().lookup(fc.getFunction()->getName());
  if (summary == nullptr) {
    // Bug fix: unannotated external functions must be treated conservatively.
    // Previously they were treated as no-ops, which is unsound: real library
    // functions (e.g. qsort, pthread_create, custom allocators) can modify
    // memory and cause pointer arguments to escape.
    //
    // Conservative model: for each pointer argument, assume the function may
    // store Universal into any memory reachable from that argument, and if the
    // call has a pointer-typed return value, assume it may return Universal.
    auto &ptrManager = globalState.getPointerManager();
    auto &env = globalState.getEnv();
    const auto *uObj = MemoryManager::getUniversalObject();
    const auto uSet = PtsSet::getSingletonSet(uObj);

    // Escape all pointer arguments: write Universal into their reachable
    // memory. Only create a new store copy if we actually need to modify it.
    Store *modifiedStore = nullptr;
    for (const auto *argVal : callNode) {
      const auto *argPtr = ptrManager.getPointer(ctx, argVal);
      if (argPtr == nullptr)
        continue;
      auto argPtsSet = env.lookup(argPtr);
      for (const auto *obj : argPtsSet) {
        if (obj->isSpecialObject())
          continue;
        // Lazily create the store copy on first modification.
        if (modifiedStore == nullptr)
          modifiedStore = &evalResult.getNewStore(*localState);
        // Weak-update all reachable pointer fields with Universal.
        auto reachable =
            globalState.getMemoryManager().getReachablePointerObjects(obj);
        for (const auto *rObj : reachable)
          modifiedStore->weakUpdate(rObj, uSet);
        modifiedStore->weakUpdate(obj, uSet);
      }
    }

    // If the call has a pointer-typed return destination, it may return
    // Universal.
    bool envChanged = false;
    if (const auto *dstVal = callNode.getDest()) {
      const auto *dstPtr = ptrManager.getOrCreatePointer(ctx, dstVal);
      envChanged = env.weakUpdate(dstPtr, uSet);
    }

    if (envChanged)
      addTopLevelSuccessors(ProgramPoint(ctx, &callNode), evalResult);
    addMemLevelSuccessors(
        ProgramPoint(ctx, &callNode),
        modifiedStore != nullptr ? *modifiedStore : *localState, evalResult);
    return;
  }

  // If the external func is a noop, we still need to propagate
  if (summary->empty()) {
    addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                          evalResult);
  } else {
    // Apply all recorded effects
    for (auto const &effect : *summary)
      evalExternalCallByEffect(ctx, callNode, effect, evalResult);
  }
}

} // namespace tpa
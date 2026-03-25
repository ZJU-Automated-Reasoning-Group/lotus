/**
 * @file ExternalLibrary.cpp
 * @brief Handling of external library function calls in constraint collection.
 *
 * This file provides support for modeling the effects of external library
 * functions (malloc, memcpy, etc.) by adding appropriate constraints based
 * on function specifications. Uses AliasSpecManager to determine function
 * categories and effects.
 *
 * @author rainoftime
 */
#include "Alias/SparrowAA/Andersen.h"
#include "Alias/SparrowAA/Log.h"
#include "Alias/Spec/AliasSpecManager.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstrTypes.h> // For CallBase
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace {
const lotus::alias::AliasSpecManager &getSpecMgr() {
  // Loads default config/{ptr,modref}.spec via AliasSpecManager.
  static lotus::alias::AliasSpecManager mgr;
  return mgr;
}

bool isEmptyModRef(const lotus::alias::ModRefInfo &mr) {
  return mr.modifiedArgs.empty() && mr.referencedArgs.empty() &&
         !mr.modifiesReturn && !mr.referencesReturn;
}
} // namespace

/**
 * @brief Add constraints for an external library function call.
 *
 * Identifies if the external call site is a library function call and adds
 * constraints accordingly. Handles allocators, memory copy functions, and
 * other known library functions based on specifications.
 *
 * If this is a call to a "known" function, adds the constraints and returns
 * true. If this is a call to an unknown function, returns false.
 *
 * @param cs The call site instruction
 * @param f The called external function
 * @param callerCtx The context of the calling function
 * @return true if the function was handled, false if unknown
 */
bool Andersen::addConstraintForExternalLibrary(
    const CallBase *cs, const Function *f,
    AndersNodeFactory::CtxKey callerCtx) {
  assert(f != nullptr && "called function is nullptr!");
  assert((f->isDeclaration() || f->isIntrinsic()) &&
         "Not an external function!");

  const auto &specMgr = getSpecMgr();

  // Quick exit: no pointer-producing effects
  auto cat = specMgr.getCategory(f);
  if (cat == lotus::alias::FunctionCategory::NoEffect ||
      cat == lotus::alias::FunctionCategory::ExitFunction) {
    return true;
  }

  bool handled = false;

  // Allocators (malloc/calloc/new/posix_memalign/...)
  if (auto allocInfo = specMgr.getAllocatorInfo(f)) {
    const Instruction *inst = cs;
    NodeIndex objIndex = nodeFactory.createObjectNode(inst, callerCtx);

    if (allocInfo->returnsPointer) {
      NodeIndex ptrIndex = nodeFactory.getValueNodeFor(inst, callerCtx);
      if (ptrIndex != AndersNodeFactory::InvalidIndex) {
        constraints.emplace_back(AndersConstraint::ADDR_OF, ptrIndex, objIndex);
        handled = true;
      }
    } else if (allocInfo->ptrOutArgIndex >= 0) {
      // B5 Fix: posix_memalign-style allocators write the address of the new
      // object through a pointer-to-pointer out-argument.  The correct
      // modelling is:
      //   *outArg = &newObj   (i.e. STORE outArg, &newObj)
      // which requires a temporary value node to hold &newObj, then a STORE
      // of that temporary through outArg.  The previous code emitted
      //   STORE outIndex, objIndex
      // which is wrong: objIndex is an *object* node, not a value node, so
      // the STORE would propagate the object's identity rather than its
      // address.  The correct sequence is:
      //   tmpPtr = &newObj   (ADDR_OF tmpPtr, objIndex)
      //   *outArg = tmpPtr   (STORE outIndex, tmpPtr)
      NodeIndex outIndex = nodeFactory.getValueNodeFor(
          cs->getArgOperand(allocInfo->ptrOutArgIndex), callerCtx);
      assert(outIndex != AndersNodeFactory::InvalidIndex &&
             "Failed to find out-arg node for allocator");
      // Create a temporary value node to represent the pointer to the new obj.
      NodeIndex tmpPtr = nodeFactory.createValueNode(nullptr, callerCtx);
      constraints.emplace_back(AndersConstraint::ADDR_OF, tmpPtr, objIndex);
      constraints.emplace_back(AndersConstraint::STORE, outIndex, tmpPtr);
      handled = true;
    }
  }

  // Memory copy-style effects (memcpy/memmove/bcopy/llvm.memcpy/...)
  auto copyEffects = specMgr.getCopyEffects(f);
  for (const auto &copy : copyEffects) {
    if (copy.dstArgIndex >= 0 && copy.srcArgIndex >= 0 && copy.dstIsRegion &&
        copy.srcIsRegion) {
      NodeIndex dstIndex = nodeFactory.getValueNodeFor(
          cs->getArgOperand(copy.dstArgIndex), callerCtx);
      NodeIndex srcIndex = nodeFactory.getValueNodeFor(
          cs->getArgOperand(copy.srcArgIndex), callerCtx);
      if (dstIndex == AndersNodeFactory::InvalidIndex ||
          srcIndex == AndersNodeFactory::InvalidIndex) {
        continue;
      }

      NodeIndex tempIndex = nodeFactory.createValueNode(nullptr, callerCtx);
      constraints.emplace_back(AndersConstraint::LOAD, tempIndex, srcIndex);
      constraints.emplace_back(AndersConstraint::STORE, dstIndex, tempIndex);
      handled = true;

      if (copy.returnsAlias) {
        NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
        if (retIndex != AndersNodeFactory::InvalidIndex) {
          NodeIndex aliased =
              (copy.retArgIndex >= 0)
                  ? nodeFactory.getValueNodeFor(
                        cs->getArgOperand(copy.retArgIndex), callerCtx)
                  : dstIndex;
          if (aliased != AndersNodeFactory::InvalidIndex)
            constraints.emplace_back(AndersConstraint::COPY, retIndex, aliased);
        }
      }
    }
  }

  // Return value aliasing (Ret aliases ArgN / STATIC / NULL)
  auto retAliases = specMgr.getReturnAliasInfo(f);
  if (!retAliases.empty()) {
    NodeIndex retIndex = nodeFactory.getValueNodeFor(cs, callerCtx);
    if (retIndex != AndersNodeFactory::InvalidIndex) {
      for (const auto &ra : retAliases) {
        if (ra.isStatic) {
          NodeIndex staticObj =
              nodeFactory.createObjectNode(nullptr, callerCtx);
          constraints.emplace_back(AndersConstraint::ADDR_OF, retIndex,
                                   staticObj);
          handled = true;
        } else if (ra.isNull) {
          constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                   nodeFactory.getNullPtrNode());
          handled = true;
        } else if (ra.argIndex >= 0 && ra.argIndex < (int)cs->arg_size()) {
          NodeIndex argIndex = nodeFactory.getValueNodeFor(
              cs->getArgOperand(ra.argIndex), callerCtx);
          if (argIndex != AndersNodeFactory::InvalidIndex) {
            constraints.emplace_back(AndersConstraint::COPY, retIndex,
                                     argIndex);
            handled = true;
          }
        }
      }
    }
  }

  if (f->getName() == "llvm.va_start") {
    const Instruction *inst = cs;
    const Function *parentF = inst->getParent()->getParent();
    assert(parentF->getFunctionType()->isVarArg());
    NodeIndex arg0Index =
        nodeFactory.getValueNodeFor(cs->getArgOperand(0), callerCtx);
    assert(arg0Index != AndersNodeFactory::InvalidIndex &&
           "Failed to find arg0 node");
    NodeIndex vaIndex = nodeFactory.getVarargNodeFor(parentF, callerCtx);
    assert(vaIndex != AndersNodeFactory::InvalidIndex &&
           "Failed to find va node");
    constraints.emplace_back(AndersConstraint::ADDR_OF, arg0Index, vaIndex);

    return true;
  }

  // If the function is marked as "ignored" in ptr.spec but has only mod/ref
  // effects (no pointer-producing effects), we also consider it handled.
  if (!handled) {
    auto mr = specMgr.getModRefInfo(f);
    if (!isEmptyModRef(mr) && (cat == lotus::alias::FunctionCategory::NoEffect))
      return true;
  }

  return handled;
}

/// @file BasicOps.cpp
/// @brief Transfer functions for basic pointer value sources in LotusAA
///
/// This file implements transfer functions for **base-case pointer sources** -
/// values that originate from allocation sites, function boundaries, or
/// constants. These are the leaf nodes in the pointer value flow graph.
///
/// **Pointer Source Categories:**
///
/// **Allocation Sites (Concrete Objects):**
/// - `alloca`: Stack-allocated objects (function-local scope)
/// - Return value from allocation functions (heap objects)
///
/// **Symbolic Objects (Function Boundary):**
/// - Function arguments: Pointers passed from callers
/// - Global variables: Program-wide static storage
///
/// **Special Values:**
/// - Null pointer: Explicit null constant
/// - Unknown: Conservative fallback for unhandled cases
/// - Non-pointer values: Values that become pointers through casts
///
/// **Object Classification:**
/// - **CONCRETE**: Allocation site known (alloca, malloc, new)
/// - **SYMBOLIC**: Value from outside function scope (args, globals)
/// - Used for inter-procedural summary generation
///
/// @see MemObject.h for object representation
/// @see processBasePointer() in PointerInstructions.cpp for dispatch logic

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;

/// Processes an alloca instruction - stack-allocated local variable.
///
/// Creates a **concrete memory object** representing a stack allocation.
/// Each alloca is allocation-site sensitive (creates unique object).
///
/// @param alloca The alloca instruction
/// @return PTResult* Singleton points-to set {&alloca}
///
/// **Example:** `int *p = alloca(int)` → p points to unique stack object
///
/// @note Alloca objects have function-local lifetime (stack frame)
PTResult *IntraLotusAA::processAlloca(AllocaInst *alloca) {
  return addPointsTo(alloca, newObject(alloca), 0, getEmptyCond());
}

/// Processes a function argument - symbolic input from caller.
///
/// Arguments are typically **symbolic objects** representing unknown caller
/// values. Exception: Pseudo-arguments (from inter-procedural analysis) are
/// CONCRETE.
///
/// @param arg The function argument
/// @return PTResult* Points-to set for the argument
///
/// **Object Kind Decision:**
/// - CONCRETE: if `func_pseudo_ret_cache.count(arg)` (output from inlined
/// callee)
/// - SYMBOLIC: otherwise (real argument from caller)
///
/// @see func_pseudo_ret_cache for pseudo-argument tracking
PTResult *IntraLotusAA::processArg(Argument *arg) {
  MemObject::ObjKind kind = func_pseudo_ret_cache.count(arg)
                                ? MemObject::CONCRETE
                                : MemObject::SYMBOLIC;
  return addPointsTo(arg, newObject(arg, kind), 0, getEmptyCond());
}

/// Processes a global variable - program-wide symbolic object.
///
/// Global variables are **symbolic** because they may be modified by other
/// translation units or initialized before analysis begins.
///
/// @param global The global variable or function
/// @return PTResult* Singleton points-to set {global_obj}
///
/// @note Global functions are also GlobalValues, handled here
PTResult *IntraLotusAA::processGlobal(GlobalValue *global) {
  return addPointsTo(global, newObject(global, MemObject::SYMBOLIC), 0,
                     getEmptyCond());
}

/// Processes a null pointer constant.
///
/// All null pointers share the singleton `NullPTS` result pointing to
/// `NullObj`.
///
/// @param null_ptr The null pointer constant
/// @return PTResult* Shared NullPTS (points to MemObject::NullObj)
///
/// @note Memory-efficient: single NullObj/NullPTS for entire program
PTResult *IntraLotusAA::processNullptr(ConstantPointerNull *null_ptr) {
  return assignPts(null_ptr, NullPTS);
}

/// Processes non-pointer values that may be used as pointers.
///
/// Handles integer-to-pointer casts and other non-standard pointer sources.
///
/// @param non_pointer_val The non-pointer value
/// @return PTResult* Derived from source if cast, otherwise new concrete object
///
/// **Cases:**
/// 1. Cast from pointer → preserve points-to relationship
/// 2. Other → create conservative concrete object
///
/// @note Handles `inttoptr` and similar cast operations
PTResult *IntraLotusAA::processNonPointer(Value *non_pointer_val) {
  if (CastInst *cast = dyn_cast<CastInst>(non_pointer_val)) {
    Value *src = cast->getOperand(0);
    if (src->getType()->isPointerTy()) {
      PTResult *src_res = processBasePointer(src);
      return derivePtsFrom(non_pointer_val, src_res, 0, getEmptyCond());
    }
  }
  return addPointsTo(non_pointer_val, newObject(non_pointer_val), 0,
                     getEmptyCond());
}

/// Fallback handler for unknown/unhandled pointer sources.
///
/// Returns conservative result pointing to `UnknownObj` - a top element
/// representing any possible memory location.
///
/// @param unknown_val The unhandled value
/// @return PTResult* Conservative result {UnknownObj}
///
/// **Soundness:** Unknown → may alias anything (conservative
/// over-approximation)
PTResult *IntraLotusAA::processUnknown(Value *unknown_val) {
  return addPointsTo(unknown_val, MemObject::UnknownObj, 0, getEmptyCond());
}

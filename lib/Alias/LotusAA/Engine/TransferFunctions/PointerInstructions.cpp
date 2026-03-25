/// @file PointerInstructions.cpp
/// @brief Transfer functions for pointer-related LLVM instructions in LotusAA
///
/// This file implements the core transfer functions that process
/// pointer-related instructions during flow-sensitive pointer analysis. It
/// handles:
///
/// **Memory Access Operations:**
/// - Load instructions: Dereference pointers and track loaded values
/// - Store instructions: Update memory with strong/weak semantics
///
/// **Control Flow Operations:**
/// - PHI nodes: Merge pointer values from multiple incoming edges
/// - Select instructions: Conditionally choose between pointer values
///
/// **Pointer Manipulation:**
/// - GetElementPtr (GEP): Field-sensitive pointer arithmetic
/// - Cast instructions: Type-preserving pointer conversions
/// - Bitcast: Reinterpret pointer types without changing address
///
/// **Design Philosophy:**
/// - Flow-sensitive: Track values at each program point
/// - Field-sensitive: Track fields via offsets (simplified to 0 in GEP)
/// - Strong updates: Overwrite values when possible
/// - SSA-based: Leverage LLVM's SSA form for efficiency
///
/// @see IntraProceduralAnalysis.h for class declaration
/// @see PointsToGraph.h for underlying points-to graph data structures

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace {

static Value *tracebackPointerCastChain(Value *ptr) {
  while (ptr) {
    if (auto *cast = dyn_cast<CastInst>(ptr)) {
      Value *src = cast->getOperand(0);
      if (!src->getType()->isPointerTy())
        break;
      ptr = src;
      continue;
    }

    if (auto *ce = dyn_cast<ConstantExpr>(ptr)) {
      if (!Instruction::isCast(ce->getOpcode()))
        break;
      Value *src = ce->getOperand(0);
      if (!src->getType()->isPointerTy())
        break;
      ptr = src;
      continue;
    }

    break;
  }

  return ptr;
}

static Type *getSequentialElementType(Type *type) {
  if (auto *array_ty = dyn_cast<ArrayType>(type))
    return array_ty->getElementType();
  if (auto *vector_ty = dyn_cast<VectorType>(type))
    return vector_ty->getElementType();
  return nullptr;
}

static int64_t getElementTypeSizeInBits(Type *type, const DataLayout &DL) {
  return type ? static_cast<int64_t>(DL.getTypeSizeInBits(type)) : 0;
}

static int64_t getLegacyStyleInboundOffset(GEPOperator *gep, unsigned start_idx,
                                           Type *start_type,
                                           const DataLayout &DL) {
  int64_t offset = 0;
  Type *type = start_type;

  for (unsigned idx = start_idx; idx < gep->getNumOperands(); ++idx) {
    Value *index_val = gep->getOperand(idx);
    if (auto *const_idx = dyn_cast<ConstantInt>(index_val)) {
      int64_t field_idx = const_idx->getSExtValue();

      if (Type *elem_type = getSequentialElementType(type)) {
        type = elem_type;
        offset += field_idx * getElementTypeSizeInBits(type, DL);
        continue;
      }

      if (auto *struct_ty = dyn_cast<StructType>(type)) {
        if (field_idx < 0 ||
            static_cast<unsigned>(field_idx) >= struct_ty->getNumElements()) {
          return PTGraph::UNKNOWN_OFFSET;
        }

        const StructLayout *layout = DL.getStructLayout(struct_ty);
        offset += static_cast<int64_t>(
            layout->getElementOffsetInBits(static_cast<unsigned>(field_idx)));
        type = struct_ty->getElementType(static_cast<unsigned>(field_idx));
        continue;
      }

      return PTGraph::UNKNOWN_OFFSET;
    }

    // Symbolic sequential indices collapse to field 0, but
    // symbolic struct indices force the whole access path to unknown.
    if (Type *elem_type = getSequentialElementType(type)) {
      type = elem_type;
      continue;
    }

    return PTGraph::UNKNOWN_OFFSET;
  }

  return offset;
}

static int64_t getLegacyStyleGepOffset(GEPOperator *gep,
                                       const DataLayout &DL) {
  Type *base_type = gep->getSourceElementType();
  if (!base_type)
    return PTGraph::UNKNOWN_OFFSET;

  int64_t pointer_offset = 0;
  if (gep->getNumOperands() >= 2) {
    Value *outer_index = gep->getOperand(1);
    if (auto *const_idx = dyn_cast<ConstantInt>(outer_index)) {
      pointer_offset = const_idx->getSExtValue() *
                       getElementTypeSizeInBits(base_type, DL);
    }
  }

  int64_t inbound_offset = 0;
  if (isa<StructType>(base_type)) {
    inbound_offset = getLegacyStyleInboundOffset(gep, 2, base_type, DL);
  } else if (Type *elem_type = getSequentialElementType(base_type)) {
    if (gep->getNumOperands() >= 3) {
      Value *inner_index = gep->getOperand(2);
      if (auto *const_idx = dyn_cast<ConstantInt>(inner_index)) {
        inbound_offset = const_idx->getSExtValue() *
                         getElementTypeSizeInBits(elem_type, DL);
      } else {
        // Symbolic array/vector indices collapse to element 0.
        inbound_offset = 0;
      }
    }
  } else {
    // Symbolic pointer arithmetic over non-composite element
    // types also collapses to offset 0 rather than a distinct unknown field.
    inbound_offset = 0;
  }

  return PTGraph::composeOffset(pointer_offset, inbound_offset);
}

static std::pair<Value *, int64_t> trackPointerOffset(Value *ptr,
                                                      const DataLayout &DL) {
  if (!ptr)
    return {nullptr, 0};

  APInt ap_offset(DL.getIndexTypeSizeInBits(ptr->getType()), 0, true);
  if (const Value *base =
          ptr->stripAndAccumulateConstantOffsets(DL, ap_offset,
                                                 /*AllowNonInbounds=*/true)) {
    if (base != ptr)
      return {const_cast<Value *>(base), ap_offset.getSExtValue() * 8};
  }

  int64_t offset = 0;

  while (true) {
    Value *ptr_start = ptr;

    while (auto *gep = dyn_cast<GEPOperator>(ptr)) {
      if (!PTGraph::isUnknownOffset(offset)) {
        offset = PTGraph::composeOffset(offset, getLegacyStyleGepOffset(gep, DL));
      }
      ptr = gep->getPointerOperand();
    }

    ptr = tracebackPointerCastChain(ptr);
    if (ptr == ptr_start)
      break;
  }

  return {ptr, offset};
}

} // namespace

//===----------------------------------------------------------------------===//
// Memory Access Operations
//===----------------------------------------------------------------------===//

/// Processes a load instruction to track pointer values read from memory.
///
/// This function implements the transfer function for load instructions. It:
/// 1. Processes the pointer operand to ensure its points-to set is computed
/// 2. If loading a pointer value, dereferences the pointer to get stored values
/// 3. Creates a points-to result for the loaded value
/// 4. Merges all possible loaded values into the result
///
/// @param load_inst The load instruction to process
///
/// @note Only processes loads of pointer type; non-pointer loads are skipped
///       after ensuring the pointer operand is analyzed
///
/// **Algorithm:**
/// ```
/// load_result = {}
/// for each location in points-to(load_ptr):
///   for each value stored at location:
///     if value is a pointer:
///       load_result = load_result ∪ points-to(value)
/// ```
///
/// @see loadPtrAt() for the memory dereferencing logic
/// @see PTResultIterator for traversing points-to sets
void IntraLotusAA::processLoad(LoadInst *load_inst) {
  Value *load_ptr = load_inst->getPointerOperand();
  processBasePointer(load_ptr);

  if (!load_inst->getType()->isPointerTy())
    return;

  mem_value_t result;
  loadPtrAt(load_ptr, load_inst, result, true);

  PTResult *load_pts = findPTResult(load_inst, true);

  for (auto &load_pair : result) {
    Value *fld_val = load_pair.val;

    if (fld_val == LocValue::FREE_VARIABLE ||
        fld_val == LocValue::UNDEF_VALUE || fld_val == LocValue::SUMMARY_VALUE)
      continue;

    PTResult *fld_pts = processBasePointer(fld_val);
    load_pts->add_derived_target(load_pair.cond, fld_pts, 0);
  }

  PTResultIterator iter(load_pts, this);
}

/// Processes a store instruction to update memory locations with new values.
///
/// This function implements the transfer function for store instructions using
/// **strong update semantics** when safe. It:
/// 1. Computes the points-to set of the destination pointer
/// 2. For each target location, stores the value operand
/// 3. If storing a pointer, ensures its points-to set is computed
///
/// @param store The store instruction to process
///
/// **Strong vs. Weak Updates:**
/// - Strong update: Overwrites previous value (must-point)
/// - Weak update: Merges with previous values (may-point)
/// - Decision made in ObjectLocator::storeValue()
///
/// **Example:**
/// ```c
/// int *p = &x;
/// *p = 42;        // Strong update to x
/// if (...) p = &y;
/// *p = 10;        // Weak update to both x and y
/// ```
///
/// @see ObjectLocator::storeValue() for update logic
/// @see processBasePointer() for pointer operand processing
void IntraLotusAA::processStore(StoreInst *store) {
  Value *ptr = store->getPointerOperand();
  Value *store_value = store->getValueOperand();
  PTResult *res = processBasePointer(ptr);
  assert(res && "Store pointer not processed");

  PTResultIterator iter(res, this);

  for (auto &pt_item : iter) {
    ObjectLocator *loc = pt_item.first;
    path_cond_t cond = pt_item.second;
    MemObject *obj = loc->getObj();
    if (obj->isNull() || obj->isUnknown())
      continue;

    loc->storeValue(store_value, store, cond, 0);
  }

  if (store_value->getType()->isPointerTy()) {
    processBasePointer(store_value);
  }
}

//===----------------------------------------------------------------------===//
// Control Flow Operations
//===----------------------------------------------------------------------===//

/// Processes a PHI node to merge pointer values from multiple control flow
/// paths.
///
/// PHI nodes represent the confluence of values from different basic blocks.
/// This function creates a points-to result that is the **union** of all
/// incoming values.
///
/// @param phi The PHI node to process
/// @return PTResult* Points-to result representing the union of all incoming
/// values
///
/// **Algorithm:**
/// ```
/// phi_pts = {}
/// for each incoming value v:
///   phi_pts = phi_pts ∪ points-to(v)
/// return phi_pts
/// ```
///
/// **Example:**
/// ```c
/// int *p;
/// if (cond) p = &x; else p = &y;
/// // PHI: p = phi [&x, BB1], [&y, BB2]
/// // Result: p may point to {x, y}
/// ```
///
/// @see add_derived_target() for merging points-to sets
PTResult *IntraLotusAA::processPhi(PHINode *phi) {
  PTResult *phi_pts = findPTResult(phi, true);

  for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
    Value *val_i = phi->getIncomingValue(i);
    PTResult *in_pts = processBasePointer(val_i);
    assert(in_pts && "PHI incoming value not processed");
    path_cond_t phi_cond =
        findOrCreateUnitPhiRegion(phi->getParent(), phi->getIncomingBlock(i));
    phi_pts->add_derived_target(phi_cond, in_pts, 0);
  }

  PTResultIterator iter(phi_pts, this);
  return phi_pts;
}

/// Processes a select instruction (ternary conditional operator).
///
/// Select instructions conditionally choose between two values based on a
/// boolean condition. Since we don't track conditions precisely, we
/// conservatively take the union of both possible values.
///
/// @param select The select instruction to process (e.g., `select i1 %cond, T*
/// %true, T* %false`)
/// @return PTResult* Points-to result unioning both branches, or nullptr if
/// non-pointer
///
/// **Semantics:** `result = cond ? true_val : false_val`
/// **Our approximation:** `result ⊇ {true_val, false_val}`
///
/// @note Only processes pointer-typed selects; returns nullptr for non-pointers
PTResult *IntraLotusAA::processSelect(SelectInst *select) {
  if (!select->getType()->isPointerTy())
    return nullptr;

  Value *true_val = select->getTrueValue();
  Value *false_val = select->getFalseValue();

  PTResult *pts_true = processBasePointer(true_val);
  PTResult *pts_false = processBasePointer(false_val);

  PTResult *select_pts = findPTResult(select, true);
  select_pts->add_derived_target(getValueCond(select->getCondition(), true),
                                 pts_true, 0);
  select_pts->add_derived_target(getValueCond(select->getCondition(), false),
                                 pts_false, 0);

  PTResultIterator iter(select_pts, this);
  return select_pts;
}

//===----------------------------------------------------------------------===//
// Pointer Manipulation Operations
//===----------------------------------------------------------------------===//

/// Processes GetElementPtr (GEP) and bitcast operations for field-sensitive
/// analysis.
///
/// This function handles pointer arithmetic and type casts, which are
/// fundamental for tracking field-level precision in structures and arrays.
///
/// @param ptr The GEP or bitcast instruction/operator to process
/// @return PTResult* Points-to result derived from the base pointer
///
/// **Offset Handling:**
/// Currently simplified - all offsets are normalized to 0. Field-sensitivity is
/// achieved through the ObjectLocator mechanism rather than offset arithmetic
/// in points-to results.
///
/// **Design Rationale:**
/// - Separates concerns: PTResult tracks objects, ObjectLocator tracks fields
/// - Simplifies points-to graph representation
/// - Field offsets handled precisely in loadPtrAt/storeValue
///
/// **Example:**
/// ```c
/// struct S { int *a; int *b; } s;
/// int **p = &s.a;  // GEP: base=s, offset=0
/// int **q = &s.b;  // GEP: base=s, offset=8
/// // Both derive points-to from 's', field resolved via ObjectLocator
/// ```
///
/// @see ObjectLocator for field-level memory modeling
PTResult *IntraLotusAA::processGepBitcast(Value *ptr) {
  auto base_off = trackPointerOffset(ptr, getDL());
  Value *base_ptr = base_off.first;
  int64_t offset = base_off.second;

  if (base_ptr == ptr) {
    return addPointsTo(ptr, newObject(ptr, MemObject::CONCRETE), 0,
                       getEmptyCond());
  }

  PTResult *pts = processBasePointer(base_ptr);
  PTResult *ret = derivePtsFrom(ptr, pts, offset, getEmptyCond());
  PTResultIterator iter(ret, this);
  return ret;
}

/// Processes pointer cast instructions (inttoptr, ptrtoint, addrspacecast,
/// etc.).
///
/// @param cast The cast instruction to process
/// @return PTResult* Points-to result derived from source operand (offset 0)
///
/// **Supported Casts:**
/// - IntToPtr/PtrToInt: Conversion between pointers and integers
/// - AddrSpaceCast: Address space conversion
/// - Other pointer casts
///
/// @note All pointer casts preserve the points-to relationship (offset = 0)
PTResult *IntraLotusAA::processCast(CastInst *cast) {
  Value *base_ptr = cast->getOperand(0);
  PTResult *pts = processBasePointer(base_ptr);
  PTResult *ret = derivePtsFrom(cast, pts, 0, getEmptyCond());
  PTResultIterator iter(ret, this);
  return ret;
}

//===----------------------------------------------------------------------===//
// Base Pointer Dispatcher
//===----------------------------------------------------------------------===//

/// Main dispatcher for processing any LLVM value as a pointer.
///
/// This is the **central entry point** for pointer analysis. It dispatches to
/// specialized transfer functions based on the value type and memoizes results.
///
/// @param base_ptr Any LLVM value that may be used as a pointer
/// @return PTResult* The points-to result for this value (never null)
///
/// **Dispatch Logic:**
/// 1. Check memoization cache (findPTResult) - return if already computed
/// 2. Dispatch to specialized handler based on value type:
///    - GEP/Bitcast → processGepBitcast()
///    - Cast → processCast()
///    - Argument → processArg()
///    - Constant Null → processNullptr()
///    - Global → processGlobal()
///    - Non-pointer → processNonPointer()
///    - Unknown → processUnknown()
///
/// **Memoization:**
/// Results are cached in `pt_results` map to ensure O(1) lookup for
/// already-processed values, critical for performance on large programs.
///
/// **Design Pattern: Visitor Pattern**
/// This function implements the visitor pattern, dispatching based on LLVM
/// value type.
///
/// @note This function guarantees to return a valid PTResult* (never null)
/// @see BasicOps.cpp for implementations of processArg, processGlobal, etc.
PTResult *IntraLotusAA::processBasePointer(Value *base_ptr) {
  PTResult *res = findPTResult(base_ptr);
  if (res)
    return res;

  if (isa<GEPOperator>(base_ptr) || isa<BitCastInst>(base_ptr)) {
    res = processGepBitcast(base_ptr);
  } else if (CastInst *cast = dyn_cast<CastInst>(base_ptr)) {
    res = processCast(cast);
  } else if (Argument *arg = dyn_cast<Argument>(base_ptr)) {
    res = processArg(arg);
  } else if (ConstantPointerNull *cnull =
                 dyn_cast<ConstantPointerNull>(base_ptr)) {
    res = processNullptr(cnull);
  } else if (GlobalValue *gv = dyn_cast<GlobalValue>(base_ptr)) {
    res = processGlobal(gv);
  } else if (ConstantExpr *ce = dyn_cast<ConstantExpr>(base_ptr)) {
    if (ce->getOpcode() == Instruction::BitCast ||
        ce->getOpcode() == Instruction::GetElementPtr)
      res = processGepBitcast(base_ptr);
  } else if (!base_ptr->getType()->isPointerTy()) {
    res = processNonPointer(base_ptr);
  }

  if (!res)
    res = processUnknown(base_ptr);

  return res;
}

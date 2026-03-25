#include "Checker/Pulse/Domain/PulseOperations.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseTaint.h"

#include <cassert>
#include <limits>
#include <set>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace pulse {

std::atomic<unsigned> AbstractValueFactory::global_next_id_{1u};

//===----------------------------------------------------------------------===//
// PulseOperations
//
// Defines the operational semantics over `AbductiveDomain`:
// - `eval` maps an LLVM value/expression to an abstract address (plus history).
// - Heap reads may *abduce* missing edges/attrs into the precondition to keep
//   witness paths sound (biabduction).
// - For access paths (GEP), we model projections precisely enough to avoid
//   conflation (critical for sound incorrectness).
//===----------------------------------------------------------------------===//

namespace {
static bool isNullPointerConstantValue(const llvm::Value *v) {
  if (!v)
    return false;
  if (llvm::isa<llvm::ConstantPointerNull>(v))
    return true;
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
    if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
      if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0)))
        return CI->isZero();
    }
  }
  if (auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(v)) {
    if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(I2P->getOperand(0)))
      return CI->isZero();
  }
  return false;
}

static llvm::Optional<int64_t> getI64Constant(const llvm::Value *v) {
  if (!v)
    return llvm::None;
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    if (CI->getBitWidth() <= 64)
      return CI->getSExtValue();
  }
  return llvm::None;
}
} // namespace

//===----------------------------------------------------------------------===//
// AbstractValueFactory Implementation
//===----------------------------------------------------------------------===//

AbstractValue AbstractValueFactory::getOrCreate(const llvm::Value *v) {
  auto it = value_map_.find(v);
  if (it != value_map_.end()) {
    return it->second;
  }
  if (mustAliasFn_) {
    for (const auto &kv : value_map_) {
      if (mustAliasFn_(v, kv.first)) {
        AbstractValue av = kv.second;
        value_map_[v] = av;
        return av;
      }
    }
  }
  unsigned id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  // 0 is reserved for the default-constructed AbstractValue.
  if (id == 0u) {
    id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  }
  AbstractValue av(v, id);
  value_map_[v] = av;
  return av;
}

AbstractValue AbstractValueFactory::get(const llvm::Value *v) const {
  auto it = value_map_.find(v);
  assert(it != value_map_.end() && "Value not found in factory");
  return it->second;
}

bool AbstractValueFactory::has(const llvm::Value *v) const {
  return value_map_.find(v) != value_map_.end();
}

AbstractValue AbstractValueFactory::createFresh(const llvm::Value *hint) {
  // Create a fresh abstract value (for unknown/allocated memory)
  // In a full implementation, we'd track this properly
  unsigned id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  if (id == 0u) {
    id = global_next_id_.fetch_add(1u, std::memory_order_relaxed);
  }
  AbstractValue av(hint, id);
  if (hint) {
    value_map_[hint] = av;
  }
  return av;
}

//===----------------------------------------------------------------------===//
// PulseOperations Implementation
//===----------------------------------------------------------------------===//

bool PulseOperations::isNullConstantSource(const Address &addr) {
  // Check if the original LLVM value is a null constant
  if (const llvm::Value *v = addr.addr.getValue()) {
    if (isNullPointerConstantValue(v))
      return true;
  }

  // Check ValueHistory for Store events where a null constant was stored
  for (const auto &event : addr.history.getEvents()) {
    if (event.kind == ValueHistory::EventKind::Store && event.location) {
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(event.location)) {
        const llvm::Value *stored_value = SI->getValueOperand();
        if (isNullPointerConstantValue(stored_value)) {
          return true;
        }
        // Also check if the stored value came from a CallInst returning null
        if (auto *CI = llvm::dyn_cast<llvm::CallInst>(stored_value)) {
          if (CI->getType()->isPointerTy()) {
            // Check if the CallInst itself is a null constant (shouldn't
            // happen, but be safe)
            if (llvm::isa<llvm::ConstantPointerNull>(CI)) {
              return true;
            }
            // Check if the called function returns null constant
            if (auto *F = CI->getCalledFunction()) {
              // Check if the function returns null constant
              for (const auto &BB : *F) {
                if (auto *RI =
                        llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
                  if (RI->getNumOperands() > 0) {
                    const llvm::Value *ret_val = RI->getReturnValue();
                    if (ret_val && ret_val->getType()->isPointerTy()) {
                      if (isNullPointerConstantValue(ret_val)) {
                        return true;
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    // Check for FunctionCall events that returned null
    if (event.kind == ValueHistory::EventKind::FunctionCall && event.location) {
      if (auto *CI = llvm::dyn_cast<llvm::CallInst>(event.location)) {
        if (CI->getType()->isPointerTy()) {
          if (auto *F = CI->getCalledFunction()) {
            // Check if the function returns null constant
            for (const auto &BB : *F) {
              if (auto *RI =
                      llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
                if (RI->getNumOperands() > 0) {
                  const llvm::Value *ret_val = RI->getReturnValue();
                  if (isNullPointerConstantValue(ret_val)) {
                    return true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  return false;
}

llvm::Optional<Address> PulseOperations::eval(AbductiveDomain &astate,
                                              const llvm::Value *exp,
                                              const llvm::Instruction *loc,
                                              const llvm::BasicBlock *pred) {
  // Global storage (GlobalVariable/Function/GlobalAlias): treat as stable
  // memory root. Mark with Attribute::Global so we can prove stack escapes like
  // `store %stack_ptr, @global` without guessing.
  if (exp && exp->getType()->isPointerTy() &&
      llvm::isa<llvm::GlobalValue>(exp)) {
    AbstractValue gv_av = factory_->getOrCreate(exp);
    astate.getPostAttrs().add(gv_av, Attribute::Global);
    return llvm::Optional<Address>(Address(gv_av));
  }

  // Pointer-typed null constants.
  if (isNullPointerConstantValue(exp)) {
    AbstractValue null_addr = factory_->createFresh(exp);
    Address addr(null_addr);
    astate.getPostAttrs().add(null_addr, Attribute::Null);
    astate.getPathFormula().addNull(null_addr);
    return llvm::Optional<Address>(addr);
  }

  // Integer constants: treat as integers, not null pointers.
  if (auto c = getI64Constant(exp)) {
    AbstractValue av = factory_->getOrCreate(exp);
    astate.getPathFormula().addIntegerConstraint(av);
    (void)astate.getPathFormula().addBounds(av, *c, *c);
    return llvm::Optional<Address>(Address(av));
  }

  if (auto *addr = astate.getPostStack().find(exp)) {
    // Canonicalize the address
    AbstractValue canon_addr = astate.getCanonical(addr->addr);
    Address result(canon_addr);
    result.history = addr->history;

    // IMPORTANT: When loading a pointer value, preserve its attributes
    // (like Invalid, Null, etc.) so they can be checked when the pointer is
    // used The attributes are already on canon_addr, so they'll be checked in
    // readDeref

    return llvm::Optional<Address>(result);
  }

  // Handle bitcast instructions - they preserve pointer identity and attributes
  if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(exp)) {
    auto src_opt = eval(astate, BC->getOperand(0), loc, pred);
    if (src_opt) {
      // Bitcast preserves the pointer value and its attributes
      // Use same canonical value as source so attributes are preserved
      AbstractValue src_canon = astate.getCanonical(src_opt->addr);
      Address result(src_canon);
      result.history = src_opt->history;
      // Attributes (Allocated, Invalid, Null) are already on src_canon, so
      // they're preserved
      return llvm::Optional<Address>(result);
    }
  }

  // Handle bitcast operator (constant expressions)
  if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(exp)) {
    if (CE->getOpcode() == llvm::Instruction::BitCast) {
      auto src_opt = eval(astate, CE->getOperand(0), loc, pred);
      if (src_opt) {
        AbstractValue src_canon = astate.getCanonical(src_opt->addr);
        Address result(src_canon);
        result.history = src_opt->history;
        // Attributes are preserved through canonical value
        return llvm::Optional<Address>(result);
      }
    }
  }

  // GEP (GetElementPtr) materializes an access path into an aggregate/array.
  //
  // Sound incorrectness principle: never silently conflate distinct
  // projections.
  // - Struct indices must be constant (LLVM requirement). If they are not,
  //   something is off; we conservatively bail out instead of inventing a
  //   projection that could hide bugs.
  // - For array/pointer indices, record the index as an integer in the path
  //   formula (and keep exact bounds for constants). This supports "proved"
  //   checks later without committing to may-alias reasoning.
  if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(exp)) {
    auto base_opt = eval(astate, GEP->getPointerOperand(), loc, pred);
    if (!base_opt)
      return llvm::None;

    // Check if base pointer is null or invalid before indexing
    AbstractValue base_canon = astate.getCanonical(base_opt->addr);
    const bool base_is_stack =
        astate.getPostAttrs().has(base_canon, Attribute::Stack);
    const bool base_is_global =
        astate.getPostAttrs().has(base_canon, Attribute::Global);
    // For NPD checker, only check Null attribute (set only by null constants)
    if (astate.getPostAttrs().has(base_canon, Attribute::Null) &&
        !astate.getPathFormula().isNonNull(base_canon)) {
      // Base is null (from null constant) - return None to signal error (will
      // be caught by caller)
      return llvm::None;
    }
    if (astate.getPostAttrs().has(base_canon, Attribute::Invalid)) {
      // Base is invalid (use-after-free) - return None to signal error
      return llvm::None;
    }

    Address base = *base_opt;
    AbstractValue cur = base.addr;
    // Reuse base_canon from above (already computed for null/invalid checks)
    bool base_is_uninitialized =
        astate.getPostAttrs().has(base_canon, Attribute::Uninitialized);

    // Best-effort: DataLayout lets us compute element stride for array
    // indexing. Stride is stored into the access key to avoid collapsing
    // different element types onto the same access path.
    const llvm::DataLayout *DL = (loc && loc->getModule())
                                     ? &loc->getModule()->getDataLayout()
                                     : nullptr;

    // Bounds tracking: use allocation size (when known) to prove OOB.
    llvm::Optional<uint64_t> alloc_size_opt =
        astate.getAllocationSize(base_canon);
    bool offset_known = true;
    int64_t total_offset = 0;
    bool oob_proven = false;

    unsigned op_index = 1;
    for (auto GTI = llvm::gep_type_begin(GEP), E = llvm::gep_type_end(GEP);
         GTI != E; ++GTI, ++op_index) {
      llvm::Value *idx = const_cast<llvm::Value *>(GEP->getOperand(op_index));
      Access acc;

      if (llvm::StructType *ST = GTI.getStructTypeOrNull()) {
        auto *C = llvm::dyn_cast<llvm::ConstantInt>(idx);
        if (!C) {
          // Struct indexing must be constant; bail out rather than conflate.
          return llvm::None;
        }
        uint64_t field = C->getZExtValue();
        if (field >= ST->getNumElements()) {
          // Invalid field access. Be conservative: treat as unknown.
          return llvm::None;
        }
        acc = Access(static_cast<unsigned>(field));

        if (offset_known && DL) {
          auto *layout = DL->getStructLayout(ST);
          uint64_t field_off = layout->getElementOffset(field);
          if (field_off >
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            offset_known = false;
          } else {
            total_offset += static_cast<int64_t>(field_off);
          }
        } else {
          offset_known = false;
        }
      } else {
        AbstractValue idxAv = factory_->getOrCreate(idx);
        astate.getPathFormula().addIntegerConstraint(idxAv);
        if (auto c = getI64Constant(idx)) {
          (void)astate.getPathFormula().addBounds(idxAv, *c, *c);
        }

        uint64_t stride_bytes = 0;
        llvm::Type *indexed = GTI.getIndexedType();
        if (DL && indexed && indexed->isSized()) {
          stride_bytes = DL->getTypeAllocSize(indexed);
        }
        acc = Access::arrayIndex(idxAv, stride_bytes);

        if (offset_known && stride_bytes > 0) {
          if (auto c = getI64Constant(idx)) {
            __int128 off =
                static_cast<__int128>(*c) * static_cast<__int128>(stride_bytes);
            if (off > std::numeric_limits<int64_t>::max() ||
                off < std::numeric_limits<int64_t>::min()) {
              offset_known = false;
            } else {
              total_offset += static_cast<int64_t>(off);
            }
          } else {
            // If we have allocation size and a symbolic index, add bounds.
            if (alloc_size_opt && offset_known) {
              uint64_t alloc_size = *alloc_size_opt;
              if (total_offset >= 0 &&
                  static_cast<uint64_t>(total_offset) < alloc_size) {
                uint64_t remaining =
                    alloc_size - static_cast<uint64_t>(total_offset);
                if (remaining > 0) {
                  uint64_t max_index = (remaining - 1) / stride_bytes;
                  (void)astate.getPathFormula().addBounds(
                      idxAv, 0, static_cast<int64_t>(max_index));
                }
              }
            }
          }
        } else {
          offset_known = false;
        }
      }

      if (auto *target = astate.getPostHeap().findEdge(cur, acc)) {
        cur = target->addr;
        // If base was uninitialized, propagate to existing field
        if (base_is_uninitialized) {
          AbstractValue target_canon = astate.getCanonical(cur);
          astate.getPostAttrs().add(target_canon, Attribute::Uninitialized);
        }
        continue;
      }
      AbstractValue fresh = factory_->createFresh(GEP);
      Address targetAddr(fresh);
      targetAddr.history = base.history;
      if (loc)
        targetAddr.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                                    loc->getFunction());
      astate.getPostHeap().addEdge(cur, acc, targetAddr);
      astate.abduceToPre(cur, acc, targetAddr);
      astate.abduceAttrToPre(cur, Attribute::Allocated);
      if (base_is_stack) {
        astate.getPostAttrs().add(fresh, Attribute::Stack);
      }
      if (base_is_global) {
        astate.getPostAttrs().add(fresh, Attribute::Global);
      }

      // Field-level initialization tracking: if base struct is uninitialized,
      // mark the field as uninitialized too
      if (base_is_uninitialized) {
        AbstractValue fresh_canon = astate.getCanonical(fresh);
        astate.getPostAttrs().add(fresh_canon, Attribute::Uninitialized);
      }

      cur = fresh;
    }
    // If we can prove an out-of-bounds offset, mark it for later reporting.
    if (alloc_size_opt && offset_known) {
      const uint64_t alloc_size = *alloc_size_opt;
      if (total_offset < 0 ||
          static_cast<uint64_t>(total_offset) >= alloc_size) {
        oob_proven = true;
      }
    }

    AbstractValue gepAv = factory_->getOrCreate(GEP);
    if (oob_proven) {
      astate.getPostAttrs().add(gepAv, Attribute::OutOfBounds);
    }
    if (alloc_size_opt && offset_known && total_offset >= 0) {
      uint64_t alloc_size = *alloc_size_opt;
      if (static_cast<uint64_t>(total_offset) < alloc_size) {
        uint64_t remaining = alloc_size - static_cast<uint64_t>(total_offset);
        if (remaining > 0) {
          astate.setAllocationSize(gepAv, remaining);
        }
      }
    }
    if (base_is_stack) {
      astate.getPostAttrs().add(gepAv, Attribute::Stack);
    }
    if (base_is_global) {
      astate.getPostAttrs().add(gepAv, Attribute::Global);
    }
    Address gepAddr(gepAv);
    gepAddr.history = base.history;
    if (loc)
      gepAddr.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                               loc->getFunction());
    Access deref(AccessKind::Dereference);
    astate.getPostHeap().addEdge(gepAv, deref, Address(cur));
    astate.abduceToPre(gepAv, deref, Address(cur));
    astate.abduceAttrToPre(gepAv, Attribute::Allocated);
    return llvm::Optional<Address>(gepAddr);
  }

  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(exp)) {
    if (!pred)
      return llvm::None;
    // Check if pred is actually a predecessor of the PHI node
    bool is_predecessor = false;
    for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
      if (Phi->getIncomingBlock(i) == pred) {
        is_predecessor = true;
        break;
      }
    }
    if (!is_predecessor) {
      // pred is not a predecessor, try to find a valid predecessor
      if (Phi->getNumIncomingValues() > 0) {
        pred = Phi->getIncomingBlock(0);
      } else {
        return llvm::None;
      }
    }
    const llvm::Value *incoming = Phi->getIncomingValueForBlock(pred);
    return eval(astate, incoming, loc, nullptr);
  }

  if (llvm::isa<llvm::Instruction>(exp)) {
    AbstractValue av = factory_->getOrCreate(exp);
    return llvm::Optional<Address>(Address(av));
  }

  AbstractValue av = factory_->getOrCreate(exp);
  return llvm::Optional<Address>(Address(av));
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::evalDeref(AbductiveDomain &astate, Address ptr,
                           const llvm::Instruction *loc) {
  // Canonicalize pointer
  AbstractValue canon_ptr = astate.getCanonical(ptr.addr);

  // PRIORITY: Check Invalid FIRST (UseAfterFree is more severe than
  // NullDereference) This prevents false positives where we report
  // NullDereference when UseAfterFree is correct
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    return {OperationResult::UseAfterFree, llvm::None};
  }

  // Check if pointer is null (using formula and attributes)
  // Only check null if Invalid is NOT present (already checked above)
  // NPD checker: only report if source is a null constant
  // Check path formula first (more precise)
  if (astate.getPathFormula().isNull(canon_ptr)) {
    // Check if this pointer originated from a null constant
    Address canon_ptr_addr(canon_ptr);
    canon_ptr_addr.history = ptr.history; // Preserve history for source check
    if (isNullConstantSource(canon_ptr_addr)) {
      return {OperationResult::NullDereference, llvm::None};
    }
  }

  // Also check attributes (may be set by comparisons)
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Null)) {
    // Double-check with formula - if formula says non-null, trust formula
    if (!astate.getPathFormula().isNonNull(canon_ptr)) {
      // Check if this pointer originated from a null constant
      Address canon_ptr_addr(canon_ptr);
      canon_ptr_addr.history = ptr.history; // Preserve history for source check
      if (isNullConstantSource(canon_ptr_addr)) {
        return {OperationResult::NullDereference, llvm::None};
      }
    }
  }

  // Try to find edge in heap (using canonical value)
  Access deref(AccessKind::Dereference);
  if (auto *target = astate.getPostHeap().findEdge(canon_ptr, deref)) {
    // Canonicalize target
    AbstractValue canon_target = astate.getCanonical(target->addr);

    // PROPAGATE ATTRIBUTES: Copy attributes from stored value to loaded value
    // This ensures that if we stored an Invalid pointer, loading it gives
    // Invalid
    auto attrs = astate.getPostAttrs().get(canon_target);
    for (Attribute attr : attrs) {
      // Attributes are already on canon_target, they will be checked by caller
    }

    Address result(canon_target);
    result.history = target->history;
    return {OperationResult::Success, result};
  }

  // Not in post heap: abduce to pre (biabduction)
  AbstractValue fresh = factory_->createFresh();
  Address target(fresh);
  target.history = ptr.history;
  target.history.addEvent(ValueHistory::EventKind::Allocation, loc,
                          loc ? loc->getFunction() : nullptr);

  // Add to post heap (using canonical value)
  astate.getPostHeap().addEdge(canon_ptr, deref, target);

  // Abduce to pre
  astate.abduceToPre(canon_ptr, deref, target);
  astate.abduceAttrToPre(canon_ptr, Attribute::Allocated);

  return {OperationResult::Success, llvm::Optional<Address>(target)};
}

OperationResult PulseOperations::checkAddrAccess(AbductiveDomain &astate,
                                                 Address addr,
                                                 const llvm::Instruction *loc) {
  // Canonicalize address
  AbstractValue canon_addr = astate.getCanonical(addr.addr);

  // PRIORITY ORDER: Invalid > Null > Uninitialized
  // Check Invalid FIRST (UseAfterFree is more severe and prevents false
  // positives)
  if (astate.getPostAttrs().has(canon_addr, Attribute::Invalid)) {
    return OperationResult::UseAfterFree;
  }

  if (astate.getPostAttrs().has(canon_addr, Attribute::OutOfBounds)) {
    return OperationResult::OutOfBounds;
  }

  // Skip null dereference check for free() calls - free(NULL) is safe in C
  bool is_free_call = false;
  if (loc) {
    if (auto *CI = llvm::dyn_cast<llvm::CallInst>(loc)) {
      if (auto *F = CI->getCalledFunction()) {
        if (F->getName() == "free") {
          is_free_call = true;
        }
      }
    }
  }

  // Check null (using formula first, then attributes)
  // Path formula is more precise (path-sensitive)
  // Only check null if Invalid is NOT present (already checked above)
  // NPD checker: only report if source is a null constant
  // Skip null check for free() calls - free(NULL) is safe
  if (!is_free_call) {
    if (astate.getPathFormula().isNull(canon_addr)) {
      if (isNullConstantSource(addr)) {
        return OperationResult::NullDereference;
      }
    }

    // Check attributes (may be set by comparisons)
    if (astate.getPostAttrs().has(canon_addr, Attribute::Null)) {
      // If formula says non-null, trust formula over attribute
      if (!astate.getPathFormula().isNonNull(canon_addr)) {
        if (isNullConstantSource(addr)) {
          return OperationResult::NullDereference;
        }
      }
    }
  }

  // Check uninitialized (for reads) - lowest priority
  // Only check if Invalid is NOT present
  if (astate.getPostAttrs().has(canon_addr, Attribute::Uninitialized)) {
    return OperationResult::UninitializedRead;
  }

  return OperationResult::Success;
}

void PulseOperations::allocate(AbductiveDomain &astate, AbstractValue addr,
                               const llvm::Instruction *loc) {
  (void)loc;
  astate.getPostAttrs().add(addr, Attribute::Allocated);
  astate.getPostAttrs().remove(addr, Attribute::Invalid);
  astate.getPostAttrs().remove(addr, Attribute::Uninitialized);
}

void PulseOperations::invalidate(AbductiveDomain &astate, Address addr,
                                 const llvm::Instruction *loc,
                                 InvalidationKind kind) {
  AbstractValue canon = astate.getCanonical(addr.addr);

  // Invalidate the canonical address
  astate.getPostAttrs().add(canon, Attribute::Invalid);
  astate.getPostAttrs().remove(canon, Attribute::Allocated);
  // Clear allocation size for freed memory.
  // (Do not retain bounds for invalidated objects.)
  // We keep it out of post state to avoid misleading OOB checks later.
  astate.setAllocationSize(canon, 0);
  astate.setInvalidationInfo(canon, kind, loc);
  // Heap edges are keyed by canonical abstract values.
  astate.getPostHeap().removeEdges(canon);

  // Also invalidate aliases - find all values that canonicalize to the same
  // value This handles cases where q = p; delete p; *q (aliased_uaf) Since
  // canonicalization already groups aliases, we just need to ensure all values
  // with the same canonical value are invalidated The canonical value itself is
  // already invalidated above

  // Propagate invalidation to aliases across stack and heap keys/values.
  std::set<AbstractValue> alias_keys;

  for (const auto &stack_kv : astate.getPostStack().getMap()) {
    AbstractValue stack_canon = astate.getCanonical(stack_kv.second.addr);
    if (stack_canon == canon) {
      alias_keys.insert(stack_canon);
    }
  }

  for (const auto &edge_kv : astate.getPostHeap().getEdges()) {
    AbstractValue source_canon = astate.getCanonical(edge_kv.first);
    if (source_canon == canon) {
      alias_keys.insert(source_canon);
    }
    for (const auto &access_kv : edge_kv.second) {
      AbstractValue target_canon = astate.getCanonical(access_kv.second.addr);
      if (target_canon == canon) {
        alias_keys.insert(target_canon);
      }
    }
  }

  for (AbstractValue alias : alias_keys) {
    astate.getPostAttrs().add(alias, Attribute::Invalid);
    astate.getPostAttrs().remove(alias, Attribute::Allocated);
    astate.setAllocationSize(alias, 0);
    astate.getPostHeap().removeEdges(alias);
  }
}

OperationResult PulseOperations::writeDeref(AbductiveDomain &astate,
                                            Address ptr, Address value,
                                            const llvm::Instruction *loc) {
  // PRIORITY: Check Invalid FIRST before other checks
  AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    return OperationResult::UseAfterFree;
  }

  // Check access (will check Null, Uninitialized, etc.)
  auto result = checkAddrAccess(astate, ptr, loc);
  if (result != OperationResult::Success) {
    return result;
  }

  // Canonicalize values (canon_ptr already defined above)
  AbstractValue canon_value = astate.getCanonical(value.addr);
  Address canon_value_addr(canon_value);
  canon_value_addr.history = value.history;

  // Write to heap (using canonical values)
  Access deref(AccessKind::Dereference);
  astate.getPostHeap().addEdge(canon_ptr, deref, canon_value_addr);

  // Propagate taint: if value is tainted, propagate to memory location
  TaintOperations::propagateThroughStore(astate, canon_value, canon_ptr, loc);

  // Initialize if needed
  initialize(astate, canon_ptr);

  // Interprocedural initialization: when writing through a pointer parameter,
  // clear Uninitialized from the pointed-to memory location
  // This handles cases like: void init(int *p) { *p = 100; } ... int x;
  // init(&x); The write initializes the memory that p points to
  AbstractValue pointed_canon = astate.getCanonical(canon_value_addr.addr);
  astate.getPostAttrs().remove(pointed_canon, Attribute::Uninitialized);

  // Also check if the pointer itself points to a stack variable
  // (for cases where we write to *p and p points to a stack variable)
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Stack)) {
    // Clear Uninitialized from the stack variable that this pointer points to
    astate.getPostAttrs().remove(canon_ptr, Attribute::Uninitialized);
  }

  return OperationResult::Success;
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::readDeref(AbductiveDomain &astate, Address ptr,
                           const llvm::Instruction *loc) {
  // PRIORITY: Check Invalid FIRST before other checks
  AbstractValue canon_ptr = astate.getCanonical(ptr.addr);
  if (astate.getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    return {OperationResult::UseAfterFree, llvm::None};
  }

  // Check access (will check Null, Uninitialized, etc.)
  auto result = checkAddrAccess(astate, ptr, loc);
  if (result != OperationResult::Success) {
    return {result, llvm::None};
  }

  // Evaluate dereference
  auto deref_result = evalDeref(astate, ptr, loc);

  // Propagate taint: if memory location is tainted, propagate to loaded value
  if (deref_result.first == OperationResult::Success && deref_result.second) {
    // canon_ptr already defined above
    AbstractValue canon_value = astate.getCanonical(deref_result.second->addr);
    TaintOperations::propagateThroughLoad(astate, canon_ptr, canon_value, loc);
  }

  return deref_result;
}

void PulseOperations::initialize(AbductiveDomain &astate, AbstractValue addr) {
  astate.getPostAttrs().remove(addr, Attribute::Uninitialized);
}

OperationResult PulseOperations::checkNull(AbductiveDomain &astate,
                                           Address addr,
                                           const llvm::Instruction *loc) {
  (void)loc;
  if (astate.getPostAttrs().has(addr.addr, Attribute::Null)) {
    return OperationResult::NullDereference;
  }
  return OperationResult::Success;
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::shallowCopy(AbductiveDomain &astate, Address source,
                             const llvm::Instruction *loc) {
  // Check source is valid
  auto result = checkAddrAccess(astate, source, loc);
  if (result != OperationResult::Success) {
    return {result, llvm::None};
  }

  // Create fresh address for copy
  AbstractValue copy_addr = factory_->createFresh();
  Address copy(copy_addr);
  copy.history = source.history;
  if (loc) {
    copy.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                          loc->getFunction());
  }

  // Copy all edges from source to copy
  AbstractValue canon_source = astate.getCanonical(source.addr);
  const auto &edges = astate.getPostHeap().getEdges();
  auto it = edges.find(canon_source);
  if (it != edges.end()) {
    for (const auto &edge_kv : it->second) {
      astate.getPostHeap().addEdge(copy_addr, edge_kv.first, edge_kv.second);
    }
  }

  // Copy attributes (except allocation - copy is a new allocation)
  const auto &attrs = astate.getPostAttrs().get(canon_source);
  for (Attribute attr : attrs) {
    if (attr != Attribute::Allocated) {
      astate.getPostAttrs().add(copy_addr, attr);
    }
  }

  return {OperationResult::Success, llvm::Optional<Address>(copy)};
}

std::pair<OperationResult, llvm::Optional<Address>>
PulseOperations::deepCopy(AbductiveDomain &astate, Address source,
                          const llvm::Instruction *loc, unsigned depth_max) {
  // Check source is valid
  auto result = checkAddrAccess(astate, source, loc);
  if (result != OperationResult::Success) {
    return {result, llvm::None};
  }

  // Create fresh address for copy
  AbstractValue copy_addr = factory_->createFresh();
  Address copy(copy_addr);
  copy.history = source.history;
  if (loc) {
    copy.history.addEvent(ValueHistory::EventKind::Unknown, loc,
                          loc->getFunction());
  }

  // Recursively copy edges up to depth_max
  AbstractValue canon_source = astate.getCanonical(source.addr);
  deepCopyRecursive(astate, canon_source, copy_addr, depth_max, 0);

  return {OperationResult::Success, llvm::Optional<Address>(copy)};
}

void PulseOperations::deepCopyRecursive(AbductiveDomain &astate,
                                        AbstractValue source,
                                        AbstractValue target,
                                        unsigned depth_max,
                                        unsigned current_depth) {
  if (depth_max > 0 && current_depth >= depth_max) {
    // At max depth, do shallow copy
    const auto &edges = astate.getPostHeap().getEdges();
    auto it = edges.find(source);
    if (it != edges.end()) {
      for (const auto &edge_kv : it->second) {
        astate.getPostHeap().addEdge(target, edge_kv.first, edge_kv.second);
      }
    }
    return;
  }

  // Deep copy: recursively copy all edges
  const auto &edges = astate.getPostHeap().getEdges();
  auto it = edges.find(source);
  if (it != edges.end()) {
    for (const auto &edge_kv : it->second) {
      AbstractValue fresh_target = factory_->createFresh();
      Address target_addr(fresh_target);
      target_addr.history = edge_kv.second.history;
      astate.getPostHeap().addEdge(target, edge_kv.first, target_addr);

      // Recursively copy
      deepCopyRecursive(astate, edge_kv.second.addr, fresh_target, depth_max,
                        current_depth + 1);
    }
  }
}

void PulseOperations::havoc(AbductiveDomain &astate, Address addr,
                            const llvm::Instruction *loc) {
  (void)loc;
  // Remove all edges from the address (model unknown effects)
  AbstractValue canon_addr = astate.getCanonical(addr.addr);
  astate.getPostHeap().removeEdges(canon_addr);

  // Remove attributes (except allocation if it was allocated)
  bool was_allocated =
      astate.getPostAttrs().has(canon_addr, Attribute::Allocated);
  astate.getPostAttrs().clear(canon_addr);
  if (was_allocated) {
    astate.getPostAttrs().add(canon_addr, Attribute::Allocated);
  }
}

OperationResult
PulseOperations::checkAddressEscape(AbductiveDomain &astate, Address addr,
                                    const llvm::Function *current_function,
                                    const llvm::Instruction *loc) {
  (void)current_function;
  (void)loc;
  // Check if address is from stack (local variable)
  // If it escapes (e.g., returned, stored to global, passed to function),
  // it should be heap-allocated

  AbstractValue canon_addr = astate.getCanonical(addr.addr);

  // Sound incorrectness: only treat as escaping stack address when provable.
  if (astate.getPostAttrs().has(canon_addr, Attribute::Stack)) {
    return OperationResult::InvalidAccess;
  }

  return OperationResult::Success;
}

} // namespace pulse

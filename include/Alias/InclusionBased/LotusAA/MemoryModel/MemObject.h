/*
 * LotusAA - Memory Object Abstraction
 * 
 * Represents abstract memory locations in pointer analysis.
 * Supports field-sensitive analysis via ObjectLocator.
 * 
 * Types:
 * - CONCRETE: Stack allocations, globals (allocation-site based)
 * - SYMBOLIC: Function arguments, return values (summary-based)
 * 
 * Structure:
 * MemObject → ObjectLocator (field at offset) → LocValue (stored values)
 */

/// @file MemObject.h
/// @brief Memory object and field-level locator abstractions
///
/// ## MemObject
///
/// Represents an abstract memory space — a stack allocation, a heap object,
/// a global variable, or a symbolic input from outside the function.
/// Each `MemObject` owns a map of `ObjectLocator` instances, one per field
/// offset, achieving **field-sensitive** tracking.
///
/// ### Object kinds
///   - **CONCRETE** — allocation site is known (alloca, malloc wrapper, global)
///   - **SYMBOLIC** (`SymbolicMemObject`) — value comes from outside the
///     function (argument, pseudo-argument); may create synthetic
///     `Argument` values for field accesses beyond what is locally defined
///
/// ### Special singletons
///   - `MemObject::NullObj` — null-pointer target
///   - `MemObject::UnknownObj` — conservative top element (any location)
///
/// ## ObjectLocator
///
/// A `(MemObject, offset)` pair representing a specific field within an
/// object.  Values stored at this location are organised in **SSA form**:
/// `LocValue` instances are grouped by defining basic block, with φ-placement
/// in dominance frontiers to correctly merge values from different paths.
///
/// ### Strong vs. Weak updates
///   - **Strong update** (`cond` is always satisfied): overwrites the previous
///     value; the dominator walk stops at the first strong update reached.
///   - **Weak update** (`cond` may not hold): merged with previous values;
///     the anti-condition (`!cond`) is accumulated so earlier definitions
///     are considered only when the weak update did not fire.
///
/// ### Value retrieval (`getValues`)
/// Walks the dominator tree upward from the query instruction, collecting all
/// reaching `LocValue` instances, stopping at strong updates and continuing
/// through weak updates with anti-condition accumulation.

#pragma once

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>

#include "Alias/InclusionBased/LotusAA/MemoryModel/Types.h"
#include "Alias/InclusionBased/LotusAA/Support/Compat.h"

namespace llvm {

// Forward declarations
class LocValue;
class MemObject;
class ObjectLocator;
class PTGraph;

// Utility to clear hash containers
template <class T> 
void lotus_clear_hash(T *to_clear) {
  T empty;
  to_clear->swap(empty);
}

// Memory value item with path conditions and confidence.
/// Memory value item — a value that was stored at a location, guarded by a
/// path condition and carrying a confidence score.
struct mem_value_item_t {
  path_cond_t cond;
  Instruction *pos;  // Where value was assigned (nullptr = from caller)
  Value *val;        // The actual value
  float confidence;

  mem_value_item_t(path_cond_t cond, Instruction *pos, Value *val,
                   float confidence = 1.0f)
      : cond(cond), pos(pos), val(val), confidence(confidence) {}

  static float compute_and_confidence(float conf1, float conf2) {
    return conf1 * conf2;
  }

  static float compute_or_confidence(float conf1, float conf2) {
    return 1.0f - ((1.0f - conf1) * (1.0f - conf2));
  }
};

using mem_value_t = std::vector<mem_value_item_t>;

/*
 * MemObject - Abstract memory space
 * 
 * Represents heap objects, stack variables, or globals.
 * Manages memory locations at different offsets.
 */
/// Abstract memory space — represents stack objects, heap objects, or global
/// variables.  Manages memory locations at different field offsets via
/// `ObjectLocator` instances.
class MemObject {
public:
  static MemObject *NullObj;
  static MemObject *UnknownObj;
  
  enum ObjKind { CONCRETE, SYMBOLIC };

protected:
  Value *alloc_site;      // Allocation site (AllocaInst, CallInst, GlobalVariable)
  PTGraph *pt_graph;      // Parent PT graph
  ObjKind obj_kind;       // CONCRETE or SYMBOLIC

  int pt_index;
  int obj_index;
  int loc_index;

  // Map offsets to locators
  std::map<int64_t, ObjectLocator *> locators;

  // Track which offsets are updated/contain pointers
  std::map<int64_t, Type *> updated_offset;
  std::map<int64_t, Type *> pointer_offset;
  
  // Cached values per offset
  std::map<int64_t, std::set<Value *>> stored_value;
  std::map<int64_t, std::set<Value *>> loaded_value;

public:
  MemObject(Value *alloc_site, PTGraph *pt_graph, ObjKind obj_kind = CONCRETE);
  virtual ~MemObject();

  static const int64_t NA = -1;

  Value *getAllocSite() { return alloc_site; }
  ObjKind getKind() const { return obj_kind; }
  PTGraph *getPTG() { return pt_graph; }
  int getPTIndex() { return pt_index; }
  int getObjIndex() { return obj_index; }

  bool isNull() { return this == NullObj; }
  bool isUnknown() { return this == UnknownObj; }
  bool isValid() { return (!isNull()) && (!isUnknown()); }

  static bool classof(const MemObject *obj) {
    return obj->getKind() == CONCRETE;
  }

  std::map<int64_t, Type *> &getUpdatedOffset() { return updated_offset; }
  std::map<int64_t, Type *> &getPointerOffset() { return pointer_offset; }
  std::map<int64_t, std::set<Value *>> &getStoredValues() { return stored_value; }
  std::map<int64_t, std::set<Value *>> &getLoadedValues() { return loaded_value; }

  void dump();
  void clear();
  Type *guessType();
  virtual std::string getName();
  ObjectLocator *findLocator(int64_t offset, bool is_create = false);
  bool isReallyAllocated();

  friend class ObjectLocator;
};

/*
 * LocValue - Value at a memory location with path conditions.
 */
/// A value at a specific memory location, guarded by a path condition.
/// Carries strong/weak update semantics for flow-sensitive analysis.
class LocValue {
public:
  enum UpdateType { STRONG, WEAK };

private:
  Value *val;
  Instruction *pos_inst;
  path_cond_t cond;
  UpdateType update_type;

public:
  // Special values
  static Value *FREE_VARIABLE;
  static Value *NO_VALUE;
  static Value *UNDEF_VALUE;
  static Value *SUMMARY_VALUE;

  LocValue(Value *val, Instruction *from_inst, path_cond_t cond,
           UpdateType update_type = STRONG)
      : val(val), pos_inst(from_inst), cond(cond), update_type(update_type) {}

  bool isStrongUpdate() { return update_type == UpdateType::STRONG; }
  void resetUpdateType(UpdateType type) { update_type = type; }
  path_cond_t getCond() { return cond; }
  void setCond(path_cond_t new_cond) { cond = new_cond; }
  Instruction *getPos() { return pos_inst; }
  Value *getVal() { return val; }
  void dump();
};

struct mem_obj_cmp {
  LLVMValueIndex *instance;
  mem_obj_cmp() : instance(LLVMValueIndex::get()) {}

  bool operator()(MemObject *A, MemObject *B) const {
    int indexPTA = A ? A->getPTIndex() : -1;
    int indexPTB = B ? B->getPTIndex() : -1;
    if (indexPTA != indexPTB || (indexPTA == -1 && indexPTB == -1)) {
      return indexPTA < indexPTB;
    }
    int indexA = A ? A->getObjIndex() : -1;
    int indexB = B ? B->getObjIndex() : -1;
    return indexA < indexB;
  }
};

/*
 * ObjectLocator - Memory location (object + offset)
 * Organizes values in SSA form
 */
/// A `(MemObject, offset)` pair representing a specific field within an
/// object.  Organises stored values in SSA form grouped by defining basic
/// block.
class ObjectLocator {
private:
  MemObject *object;
  int64_t offset;
  int load_level;
  int store_level;
  int obj_index;
  int loc_index;

  // Values grouped by basic blocks (SSA)
  std::map<BasicBlock *, std::vector<LocValue *>, llvm_cmp> loc_values;

public:
  static const int FUNC_LEVEL_UNDEFINED = -1;

  int getLoadFunctionLevel() {
    return (load_level == FUNC_LEVEL_UNDEFINED) ? 0 : load_level;
  }

  int getStoreFunctionLevel() {
    return (store_level == FUNC_LEVEL_UNDEFINED) ? 0 : store_level;
  }

  ObjectLocator(MemObject *obj, int64_t off);
  ObjectLocator(const ObjectLocator &locator);
  ~ObjectLocator();

  MemObject *getObj() const { return object; }
  int64_t getOffset() const { return offset; }
  PTGraph *getPTG();
  int getObjIndex() { return obj_index; }
  int getLocIndex() { return loc_index; }

  void dump();

  ObjectLocator *offsetBy(int64_t extra_off);
  LocValue *storeValue(Value *val, Instruction *inst, path_cond_t cond,
                       int function_level = FUNC_LEVEL_UNDEFINED);
  
  // Get values from locator with path and summary bookkeeping.
  Argument *getValues(Instruction *pos_inst, path_cond_t pre_cond,
                      mem_value_t &res,
                      Type *symbol_type = nullptr,
                      int function_level = FUNC_LEVEL_UNDEFINED,
                      bool enable_strong_update = true,
                      ObjectLocator *func_call_cache = nullptr,
                      bool is_include_func_summary = false,
                      const std::set<Instruction *, llvm_cmp>
                          *surviving_store_positions = nullptr);

  Value *getInitializerForGlobalValue();
  LocValue *getVersion(Instruction *pos_inst);

  friend raw_ostream &operator<<(raw_ostream &out, ObjectLocator &locator);

private:
  std::vector<LocValue *> *getValueList(BasicBlock *);
  void placePhi(LocValue *loc_value, BasicBlock *def_bb);
};

struct obj_loc_cmp {
  LLVMValueIndex *instance;
  obj_loc_cmp() : instance(LLVMValueIndex::get()) {}

  bool operator()(ObjectLocator *A, ObjectLocator *B) const {
    int indexObjA = A ? A->getObjIndex() : -1;
    int indexObjB = B ? B->getObjIndex() : -1;
    if (indexObjA != indexObjB || (indexObjA == -1 && indexObjB == -1)) {
      return indexObjA < indexObjB;
    }
    int indexA = A ? A->getLocIndex() : -1;
    int indexB = B ? B->getLocIndex() : -1;
    return indexA < indexB;
  }
};

/*
 * SymbolicMemObject - Represents function input memory
 */
/// Symbolic memory object — represents memory from outside function scope
/// (e.g., function arguments).  May create pseudo-arguments for inaccessible
/// field accesses, enabling inter-procedural side-effect tracking.
class SymbolicMemObject : public MemObject {
private:
  std::map<ObjectLocator *, Argument *, obj_loc_cmp> pseudo_args;

public:
  SymbolicMemObject(Value *alloc_site, PTGraph *pt_graph)
      : MemObject(alloc_site, pt_graph, MemObject::SYMBOLIC) {}

  ~SymbolicMemObject();

  bool isPseudoArgHeap() {
    if (Argument *arg_ptr = dyn_cast<Argument>(alloc_site))
      return arg_ptr->getParent() == nullptr;
    return false;
  }

  std::string getName() override;
  Argument *findCreatePseudoArg(ObjectLocator *loc, Type *arg_type);

  static bool classof(const MemObject *obj) {
    return obj->getKind() == SYMBOLIC;
  }

  const std::map<ObjectLocator *, Argument *, obj_loc_cmp> &getPseudoArgs() {
    return pseudo_args;
  }
};

} // namespace llvm

//===- AbstractState.h ----Abstract State--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
// This file defines the AbstractState class, which represents the abstract
// execution state during program analysis. The state maps program variables
// and memory locations to abstract values (intervals or addresses).
//
// Key concepts:
// - Abstract values: Either intervals (e.g., [0, 100]) or address sets
// - Variable mapping: Maps SSA values to their abstract values
// - Memory mapping: Maps heap/stack objects to their abstract values
// - Field sensitivity: Tracks individual struct fields via GEP offsets
//
// Based on the paper:
// "Precise Sparse Abstract Execution via Cross-Domain Interaction"
// Xiao Cheng, Jiawei Wang, Yulei Sui. ICSE 2024.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractValue.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

// Forward declaration (SVFIRWrapper only needs pointer)
class SVFIRWrapper;

/// Maximum field limit for GEP offset calculations to prevent explosion
/// in field-sensitive analysis. Fields beyond this limit are merged.
[[maybe_unused]] static constexpr uint32_t MaxFieldLimit = 10000;

/// AbstractState represents the abstract execution state at a program point.
///
/// The state consists of two main mappings:
/// 1. Variable-to-value map (_varToAbsVal): Maps SSA value IDs to abstract
/// values
/// 2. Address-to-value map (_addrToAbsVal): Maps memory object IDs to abstract
/// values
///
/// Abstract values can be:
/// - Intervals: Numeric ranges like [0, 100] or [-∞, +∞]
/// - Addresses: Sets of memory object IDs that a pointer may point to
///
/// Example:
///   int x = 5;           // _varToAbsVal[x_id] = IntervalValue(5, 5)
///   int *p = &x;         // _varToAbsVal[p_id] = AddressValue({x_obj_id})
///   int arr[10];         // _addrToAbsVal[arr_obj_id] = IntervalValue::top()
///   free(p);             // _freedAddrs.insert(x_obj_id)
///
class AbstractState {
public:
  /// Map from variable ID to abstract value
  typedef std::unordered_map<uint32_t, AbstractValue> VarToAbsValMap;

  /// Map from memory address/object ID to abstract value
  typedef VarToAbsValMap AddrToAbsValMap;

  /// Special sentinel IDs for null and black-hole pointers
  static constexpr uint32_t NullPtr = 0; ///< Null pointer constant (ID 0)
  static constexpr uint32_t BlkPtr =
      2; ///< Black-hole pointer (absorbs all unknown pointers)

  /// Set of freed memory object IDs (for use-after-free detection)
  std::unordered_set<uint32_t> _freedAddrs;
  std::unordered_set<uint32_t> _pendingFreedAddrs;
  std::unordered_set<uint32_t> _heapObjs;

  /// Maps SSA value IDs to their abstract values (intervals or addresses)
  VarToAbsValMap _varToAbsVal;

  /// Maps memory object IDs to their abstract values (heap/stack contents)
  AddrToAbsValMap _addrToAbsVal;

  /// Pointer to SVFIR wrapper for querying pointer analysis results
  SVFIRWrapper *svfir_ = nullptr;

  /// Default constructor - creates an empty abstract state
  AbstractState() {}

  /// Construct from existing variable and address maps
  AbstractState(VarToAbsValMap &_varToValMap, AddrToAbsValMap &_locToValMap)
      : _varToAbsVal(_varToValMap), _addrToAbsVal(_locToValMap) {}

  /// Copy constructor - performs deep copy of all state components
  AbstractState(const AbstractState &rhs)
      : _freedAddrs(rhs._freedAddrs), _varToAbsVal(rhs._varToAbsVal),
        _pendingFreedAddrs(rhs._pendingFreedAddrs), _heapObjs(rhs._heapObjs),
        _addrToAbsVal(rhs._addrToAbsVal), svfir_(rhs.svfir_),
        _objToSize(rhs._objToSize),
        _gepObjOffsetFromBase(rhs._gepObjOffsetFromBase),
        _gepFieldObjMap(rhs._gepFieldObjMap),
        _nextGepFieldId(rhs._nextGepFieldId) {}

  virtual ~AbstractState() = default;

  /// Copy assignment operator
  AbstractState &operator=(const AbstractState &rhs) {
    if (rhs != *this) {
      _varToAbsVal = rhs._varToAbsVal;
      _addrToAbsVal = rhs._addrToAbsVal;
      _freedAddrs = rhs._freedAddrs;
      _pendingFreedAddrs = rhs._pendingFreedAddrs;
      _heapObjs = rhs._heapObjs;
      _objToSize = rhs._objToSize;
      _gepObjOffsetFromBase = rhs._gepObjOffsetFromBase;
      _gepFieldObjMap = rhs._gepFieldObjMap;
      _nextGepFieldId = rhs._nextGepFieldId;
      svfir_ = rhs.svfir_;
    }
    return *this;
  }

  /// Move constructor - transfers ownership of state components
  AbstractState(AbstractState &&rhs)
      : _freedAddrs(std::move(rhs._freedAddrs)),
        _pendingFreedAddrs(std::move(rhs._pendingFreedAddrs)),
        _heapObjs(std::move(rhs._heapObjs)),
        _varToAbsVal(std::move(rhs._varToAbsVal)),
        _addrToAbsVal(std::move(rhs._addrToAbsVal)), svfir_(rhs.svfir_),
        _objToSize(std::move(rhs._objToSize)),
        _gepObjOffsetFromBase(std::move(rhs._gepObjOffsetFromBase)),
        _gepFieldObjMap(std::move(rhs._gepFieldObjMap)),
        _nextGepFieldId(rhs._nextGepFieldId) {}

  /// Move assignment operator
  AbstractState &operator=(AbstractState &&rhs) {
    if (&rhs != this) {
      _varToAbsVal = std::move(rhs._varToAbsVal);
      _addrToAbsVal = std::move(rhs._addrToAbsVal);
      _freedAddrs = std::move(rhs._freedAddrs);
      _pendingFreedAddrs = std::move(rhs._pendingFreedAddrs);
      _heapObjs = std::move(rhs._heapObjs);
      _objToSize = std::move(rhs._objToSize);
      _gepObjOffsetFromBase = std::move(rhs._gepObjOffsetFromBase);
      _gepFieldObjMap = std::move(rhs._gepFieldObjMap);
      _nextGepFieldId = rhs._nextGepFieldId;
      svfir_ = rhs.svfir_;
    }
    return *this;
  }

  /// Access abstract value for a variable (mutable)
  AbstractValue &operator[](uint32_t varId) { return _varToAbsVal[varId]; }

  /// Access abstract value for a variable (const)
  const AbstractValue &operator[](uint32_t varId) const {
    return _varToAbsVal.at(varId);
  }

  /// Check if a variable ID maps to an address value (pointer)
  /// @param id Variable ID to check
  /// @return true if the variable holds an address set, false otherwise
  bool inVarToAddrsTable(uint32_t id) const {
    if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
      if (_varToAbsVal.at(id).isAddr()) {
        return true;
      }
    }
    return false;
  }

  /// Check if a variable ID maps to an interval value (numeric)
  /// @param id Variable ID to check
  /// @return true if the variable holds an interval, false otherwise
  bool inVarToValTable(uint32_t id) const {
    if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
      if (_varToAbsVal.at(id).isInterval()) {
        return true;
      }
    }
    return false;
  }

  /// Check if a memory address maps to an address value (pointer stored in
  /// memory)
  /// @param id Memory object ID to check
  /// @return true if the memory location holds an address set, false otherwise
  bool inAddrToAddrsTable(uint32_t id) const {
    if (_addrToAbsVal.find(id) != _addrToAbsVal.end()) {
      if (_addrToAbsVal.at(id).isAddr()) {
        return true;
      }
    }
    return false;
  }

  /// Check if a memory address maps to an interval value (numeric stored in
  /// memory)
  /// @param id Memory object ID to check
  /// @return true if the memory location holds an interval, false otherwise
  bool inAddrToValTable(uint32_t id) const {
    if (_addrToAbsVal.find(id) != _addrToAbsVal.end()) {
      if (_addrToAbsVal.at(id).isInterval()) {
        return true;
      }
    }
    return false;
  }

  /// Get the variable-to-value mapping (read-only)
  const VarToAbsValMap &getVarToVal() const { return _varToAbsVal; }

  /// Get the address-to-value mapping (read-only)
  const AddrToAbsValMap &getLocToVal() const { return _addrToAbsVal; }

  /// Widening operator: over-approximates the join to ensure termination
  /// Used in fixpoint iteration to accelerate convergence by extrapolating
  /// trends. Example: [0,10] ⊔ [0,20] with widening → [0,+∞]
  AbstractState widening(const AbstractState &other);

  /// Narrowing operator: refines over-approximations from widening
  /// Used after widening reaches fixpoint to improve precision.
  /// Example: [0,+∞] ⊓ [0,100] with narrowing → [0,100]
  AbstractState narrowing(const AbstractState &other);

  /// Join this state with another (in-place union/least upper bound)
  /// Merges two abstract states by taking the join of all abstract values.
  void joinWith(const AbstractState &other);

  /// Meet this state with another (in-place intersection/greatest lower bound)
  /// Refines this state by taking the meet of all abstract values.
  void meetWith(const AbstractState &other);

  /// Mark a memory address as freed (for use-after-free detection)
  /// @param addr Memory object ID to mark as freed
  void addToFreedAddrs(uint32_t addr) {
    if (AddressValue::isVirtualMemAddress(addr)) {
      _freedAddrs.insert(AddressValue::getInternalID(addr));
    } else {
      _freedAddrs.insert(addr);
    }
  }

  void addToPendingFreedAddrs(uint32_t addr) {
    if (AddressValue::isVirtualMemAddress(addr)) {
      _pendingFreedAddrs.insert(AddressValue::getInternalID(addr));
    } else {
      _pendingFreedAddrs.insert(addr);
    }
  }

  void commitPendingFrees() {
    _freedAddrs.insert(_pendingFreedAddrs.begin(), _pendingFreedAddrs.end());
    _pendingFreedAddrs.clear();
  }

  void clearPendingFrees() { _pendingFreedAddrs.clear(); }

  void addHeapObject(uint32_t objId) { _heapObjs.insert(objId); }

  bool isHeapObject(uint32_t objId) const {
    return _heapObjs.find(objId) != _heapObjs.end();
  }

  /// Check if a memory address has been freed
  /// @param addr Memory object ID to check
  /// @return true if the address was previously freed, false otherwise
  bool isFreedMem(uint32_t addr) const {
    uint32_t objId = AddressValue::isVirtualMemAddress(addr)
                         ? AddressValue::getInternalID(addr)
                         : addr;
    return _freedAddrs.find(objId) != _freedAddrs.end();
  }

  /// Store an abstract value to a memory address
  /// @param addr Virtual memory address (encoded object ID)
  /// @param val Abstract value to store
  /// Skips stores to null memory or freed memory (for safety)
  void store(uint32_t addr, const AbstractValue &val) {
    if (!AddressValue::isVirtualMemAddress(addr))
      return;
    uint32_t objId = getIDFromAddr(addr);
    if (isNullMem(addr))
      return;
    // Check for freed memory before storing
    if (isFreedMem(addr))
      return;
    _addrToAbsVal[objId] = val;
  }

  /// Load an abstract value from a memory address
  /// @param addr Virtual memory address (encoded object ID)
  /// @return Reference to the abstract value at that address
  AbstractValue &load(uint32_t addr) {
    assert(AddressValue::isVirtualMemAddress(addr) && "not virtual address?");
    uint32_t objId = getIDFromAddr(addr);
    return _addrToAbsVal[objId];
  }

  /// Print the abstract state to stderr (for debugging)
  void printAbstractState() const;

  /// Convert state to string representation
  std::string toString() const { return ""; }

  /// Check if this state equals another state
  bool equals(const AbstractState &other) const;

  /// Compute hash of this state
  uint32_t hash() const;

  static bool eqVarToValMap(const VarToAbsValMap &lhs,
                            const VarToAbsValMap &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &item : lhs) {
      auto it = rhs.find(item.first);
      if (it == rhs.end())
        return false;
      if (!item.second.equals(it->second))
        return false;
    }
    return true;
  }

  static bool geqVarToValMap(const VarToAbsValMap &lhs,
                             const VarToAbsValMap &rhs) {
    if (rhs.empty())
      return true;
    for (const auto &item : rhs) {
      auto it = lhs.find(item.first);
      if (it == lhs.end())
        return false;
      if (!it->second.getInterval().contain(item.second.getInterval()))
        return false;
    }
    return true;
  }

  static bool geqFreedAddrs(const std::unordered_set<uint32_t> &lhs,
                            const std::unordered_set<uint32_t> &rhs) {
    for (uint32_t addr : rhs) {
      if (lhs.find(addr) == lhs.end())
        return false;
    }
    return true;
  }

  static bool geqObjSizeMap(const std::unordered_map<uint32_t, uint32_t> &lhs,
                            const std::unordered_map<uint32_t, uint32_t> &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &item : rhs) {
      auto it = lhs.find(item.first);
      if (it == lhs.end())
        return false;
      // Treat object size as exact metadata for lattice-order checks.
      if (it->second != item.second)
        return false;
    }
    return true;
  }

  bool operator==(const AbstractState &rhs) const {
    return eqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
           eqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
           _freedAddrs == rhs._freedAddrs && _objToSize == rhs._objToSize;
  }

  bool operator!=(const AbstractState &rhs) const { return !(*this == rhs); }

  bool operator<(const AbstractState &rhs) const { return !(*this >= rhs); }

  bool operator>=(const AbstractState &rhs) const {
    return geqVarToValMap(_varToAbsVal, rhs.getVarToVal()) &&
           geqVarToValMap(_addrToAbsVal, rhs.getLocToVal()) &&
           geqFreedAddrs(_freedAddrs, rhs._freedAddrs) &&
           geqObjSizeMap(_objToSize, rhs._objToSize);
  }

  void clear() {
    _addrToAbsVal.clear();
    _varToAbsVal.clear();
    _freedAddrs.clear();
  }

  AbstractState bottom() const {
    AbstractState inv = *this;
    for (auto &item : inv._varToAbsVal) {
      if (item.second.isInterval())
        item.second.getInterval().set_to_bottom();
    }
    return inv;
  }

  AbstractState top() const {
    AbstractState inv = *this;
    for (auto &item : inv._varToAbsVal) {
      if (item.second.isInterval())
        item.second.getInterval().set_to_top();
    }
    return inv;
  }

  static inline bool isNullMem(uint32_t addr) { return addr == NullMemAddr; }
  static inline bool isInvalidMem(uint32_t addr) {
    return addr == InvalidMemAddr;
  }

  static inline uint32_t getVirtualMemAddress(uint32_t idx) {
    return AddressValue::getVirtualMemAddress(idx);
  }

  static inline bool isVirtualMemAddress(uint32_t val) {
    return AddressValue::isVirtualMemAddress(val);
  }

  inline uint32_t getIDFromAddr(uint32_t addr) const {
    uint32_t objId = AddressValue::getInternalID(addr);
    return _freedAddrs.count(objId)
               ? AddressValue::getInternalID(InvalidMemAddr)
               : objId;
  }

  AddressValue getGepObjAddrs(uint32_t pointer, IntervalValue offset);
  AbstractValue loadValue(uint32_t varId);
  void storeValue(uint32_t varId, uint32_t valId);

  // GEP offset computation
  IntervalValue getByteOffset(const llvm::GetElementPtrInst *gep);
  IntervalValue getElementIndex(const llvm::GetElementPtrInst *gep);
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca);
  uint32_t getAllocaInstByteSize(const llvm::AllocaInst *alloca,
                                 const AbstractState &as);

  AddressValue getGepObjAddrs(uint32_t pointer, IntervalValue offset,
                              const llvm::GetElementPtrInst *gep);
  uint32_t getGepFieldSize(llvm::Type *srcType, int64_t offset,
                           const llvm::DataLayout &dl);

  // Object initialization
  void initObjVar(const llvm::Value *objVar);

  // Pointer to SVFIRWrapper for PTA-based queries
  void setSVFIRWrapper(SVFIRWrapper *wrapper) { svfir_ = wrapper; }
  SVFIRWrapper *getSVFIRWrapper() const { return svfir_; }

  // Type queries using PTA
  const llvm::Type *getPointeeElement(uint32_t id);

  // Get object size using PTA
  uint32_t getObjectSize(const llvm::Value *obj) const;

  // Get points-to set for a pointer using PTA
  // Returns vector of objects (as void* to avoid template in header)
  void getPointsToSet(const llvm::Value *ptr,
                      std::vector<void *> &result) const;

  // Object size tracking (public for AE use)
  void setObjSize(uint32_t objId, uint32_t size) { _objToSize[objId] = size; }
  uint32_t getObjSize(uint32_t objId) const {
    auto it = _objToSize.find(objId);
    return it != _objToSize.end() ? it->second : 0;
  }

private:
  // Object size tracking (storage)
  std::unordered_map<uint32_t, uint32_t> _objToSize;

  // GEP object offset tracking (similar to SVF's GepObjVar)
  // Maps GEP instruction pointer -> offset from base object
  std::unordered_map<const llvm::GetElementPtrInst *, IntervalValue>
      _gepObjOffsetFromBase;

  // GEP field-sensitive object tracking: maps (baseObjId, offset) -> field
  // object ID This maintains field sensitivity similar to SVF's getGepObjVar()
  std::map<std::pair<uint32_t, int64_t>, uint32_t> _gepFieldObjMap;
  uint32_t _nextGepFieldId =
      0x80000000; // Start from high IDs to avoid collision

  void setGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep,
                               const IntervalValue &offset) {
    _gepObjOffsetFromBase[gep] = offset;
  }
  bool hasGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const {
    return _gepObjOffsetFromBase.find(gep) != _gepObjOffsetFromBase.end();
  }
  IntervalValue
  getGepObjOffsetFromBase(const llvm::GetElementPtrInst *gep) const {
    auto it = _gepObjOffsetFromBase.find(gep);
    if (it != _gepObjOffsetFromBase.end()) {
      return it->second;
    }
    return IntervalValue(0, 0);
  }

  // Get or create a field-sensitive GEP object ID (matching SVF's getGepObjVar)
  uint32_t getGepFieldObjId(uint32_t baseObjId, int64_t offset) {
    auto key = std::make_pair(baseObjId, offset);
    auto it = _gepFieldObjMap.find(key);
    if (it != _gepFieldObjMap.end()) {
      return it->second;
    }
    uint32_t newId = _nextGepFieldId++;
    _gepFieldObjMap[key] = newId;
    return newId;
  }

  AbstractState sliceState(std::set<uint32_t> &sl) const {
    AbstractState inv;
    for (uint32_t id : sl) {
      if (_varToAbsVal.find(id) != _varToAbsVal.end()) {
        inv._varToAbsVal[id] = _varToAbsVal.at(id);
      }
    }
    return inv;
  }
};

} // namespace analysis
} // namespace lotus

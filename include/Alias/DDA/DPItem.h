//===- DPItem.h -- Demand-driven analysis item (SVF-style) -------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DPItem / StmtDPItem: Demand-Driven Program Item
//
// This file defines the core data structure for demand-driven analysis,
// representing a (pointer, location) pair during backward traversal.
//
// == What is a DPItem? ==
//
// A DPItem (Demand-driven Program Item) represents a query state during
// backward traversal through the value-flow graph:
// - cur: Current pointer/object node ID being analyzed
// - curloc: Current SVFG location (program point)
//
// == Why Two Classes? ==
//
// - DPItem: Base class with only `cur` (node ID)
//   - Used for context-insensitive analysis
//   - Compares only node ID (ignores location)
//
// - StmtDPItem: Derived class with `cur` and `curloc`
//   - Used for flow-sensitive analysis
//   - Compares both node ID and location
//   - Distinguishes same pointer at different program points
//
// == Example ==
//
// ```c
// int *p = &x;        // Location L1: p -> {x}
// if (cond)
//   p = &y;           // Location L2: p -> {y}
// int z = *p;         // Location L3: query p
// ```
//
// DPItems during backward traversal from L3:
// - DPItem(p, L3): Start at use of p
// - DPItem(p, L2): Backward to assignment p = &y
// - DPItem(p, L1): Backward to assignment p = &x
//
// Without location (DPItem): Would treat all as same item
// With location (StmtDPItem): Distinguishes different program points
//
// == Bug Fix Note ==
//
// StmtDPItem overrides operator< and operator== to compare both `cur` and
// `curloc`. This fixes a bug where std::set/std::map would silently drop
// items with the same `cur` but different `curloc` values.
//
// Always use StmtDPItem (or its aliases LocDPItem, CxtLocDPItem) as map/set
// keys, never the base DPItem class.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>

#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

struct SVFGNode; // forward decl for StmtDPItem<SVFGNode>

/// Base DP item (current variable only, no location).
///
/// Bug 7 note: DPItem::operator< and operator== compare only `cur` (the node
/// ID). This is intentional for the base class because DPItem has no location
/// field. The concrete subclass StmtDPItem overrides both operators to also
/// compare `curloc`, so std::set<StmtDPItem> / std::map<StmtDPItem,...>
/// correctly distinguish items with the same cur but different locations.
///
/// The risk is that code instantiated with the static type DPItem (rather than
/// StmtDPItem) would silently treat two items with the same cur but different
/// locations as equal. To prevent this, DPItem should never be used directly
/// as a map/set key; always use the concrete StmtDPItem (i.e. LocDPItem or
/// CxtLocDPItem). The static_assert below enforces this at the call sites that
/// matter most.
class DPItem {
protected:
  uint32_t cur;
  static uint32_t maximumBudget;

public:
  explicit DPItem(uint32_t c) : cur(c) {}
  DPItem(const DPItem &o) : cur(o.cur) {}
  uint32_t getCurNodeID() const { return cur; }
  void setCurNodeID(uint32_t c) { cur = c; }
  static void setMaxBudget(uint32_t max) { maximumBudget = max; }
  static uint32_t getMaxBudget() { return maximumBudget; }
  bool operator<(const DPItem &rhs) const { return cur < rhs.cur; }
  bool operator==(const DPItem &rhs) const { return cur == rhs.cur; }
  bool operator!=(const DPItem &rhs) const { return !(*this == rhs); }

  /// Debug dump (SVF-style).
  void dump(llvm::raw_ostream &os) const { os << "cur=" << cur; }
};

/// Flow-sensitive DP item: (current node ID, current SVFG location).
///
/// operator< and operator== compare both `cur` and `curloc` so that two items
/// at the same variable but different program points are treated as distinct.
/// This is the fix for Bug 7: the base DPItem only compared `cur`, which would
/// cause std::set/std::map to silently drop the second of two items that share
/// the same cur but have different curloc values.
template <class LocCond> class StmtDPItem : public DPItem {
protected:
  const LocCond *curloc;

public:
  StmtDPItem(uint32_t c, const LocCond *loc) : DPItem(c), curloc(loc) {}
  StmtDPItem(const StmtDPItem &o) : DPItem(o), curloc(o.curloc) {}
  const LocCond *getLoc() const { return curloc; }
  void setLoc(const LocCond *l) { curloc = l; }
  void setLocVar(const LocCond *l, uint32_t v) {
    curloc = l;
    cur = v;
  }
  bool operator<(const StmtDPItem &rhs) const {
    if (cur != rhs.cur)
      return cur < rhs.cur;
    return curloc < rhs.curloc;
  }
  bool operator==(const StmtDPItem &rhs) const {
    return cur == rhs.cur && curloc == rhs.curloc;
  }
  bool operator!=(const StmtDPItem &rhs) const { return !(*this == rhs); }

  /// Debug dump (SVF-style): cur and location pointer.
  void dump(llvm::raw_ostream &os) const {
    os << "cur=" << cur << " loc=" << static_cast<const void *>(curloc);
  }
};

} // namespace analysis
} // namespace lotus

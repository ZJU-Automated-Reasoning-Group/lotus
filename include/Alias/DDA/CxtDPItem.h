//===- CxtDPItem.h -- Context-sensitive DP item (SVF-style) -------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// Context condition and context-sensitive DP items for ContextDDA.
// CallStrCxt = call-string context (sequence of call site IDs).
// CxtVar = (ContextCond, node ID). CxtPtSet = set of CxtVar.
// CxtStmtDPItem = StmtDPItem + ContextCond for flow- and context-sensitive DDA.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DPItem.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

struct SVFGNode;

/// Call-string context: sequence of call site IDs (e.g. ICFG call node IDs).
using CallStrCxt = std::vector<uint32_t>;

/// Context condition (call-string) for context-sensitive DDA.
class ContextCond {
public:
  ContextCond() : concreteCxt_(true) {}
  ContextCond(const ContextCond &o)
      : context_(o.context_), concreteCxt_(o.concreteCxt_) {}
  ContextCond &operator=(const ContextCond &o) {
    if (this != &o) {
      context_ = o.context_;
      concreteCxt_ = o.concreteCxt_;
    }
    return *this;
  }

  const CallStrCxt &getContexts() const { return context_; }
  CallStrCxt &getContexts() { return context_; }
  bool isConcreteCxt() const { return concreteCxt_; }
  void setNonConcreteCxt() { concreteCxt_ = false; }
  bool containCallStr(uint32_t cxt) const {
    return std::find(context_.begin(), context_.end(), cxt) != context_.end();
  }
  size_t cxtSize() const { return context_.size(); }

  static void setMaxCxtLen(uint32_t max) { maximumCxtLen = max; }
  static void setMaxPathLen(uint32_t max) { maximumPathLen = max; }
  static uint32_t getMaxCxtLen() { return maximumCxtLen; }
  static uint32_t getMaxPathLen() { return maximumPathLen; }

  /// Push call site id; return false if context limit exceeded (then may set
  /// non-concrete).
  bool pushContext(uint32_t ctx);
  /// Match and pop (for return edge); return true if back() == ctx and pop.
  bool matchContext(uint32_t ctx);
  /// Pop back of context (e.g. for recursion: pop recursive call sites).
  void popBack();

  bool operator<(const ContextCond &rhs) const {
    return context_ < rhs.context_;
  }
  bool operator==(const ContextCond &rhs) const {
    return context_ == rhs.context_;
  }
  bool operator!=(const ContextCond &rhs) const { return !(*this == rhs); }
  std::string toString() const;

  static uint32_t maximumCxtLen;
  static uint32_t maximumPathLen;

  /// Maximum context length observed during analysis (for statistics).
  static uint32_t maxCxtLenSeen;
  static uint32_t getMaxCxtLenSeen() { return maxCxtLenSeen; }
  static void updateMaxCxtLenSeen(size_t len);

private:
  CallStrCxt context_;
  bool concreteCxt_;
};

/// Context-sensitive variable (context + node ID). Used as points-to element in
/// ContextDDA.
class CxtVar {
public:
  CxtVar() : cond_(), id_(0) {}
  CxtVar(const ContextCond &cond, uint32_t id) : cond_(cond), id_(id) {}
  CxtVar(const CxtVar &o) : cond_(o.cond_), id_(o.id_) {}
  CxtVar &operator=(const CxtVar &o) {
    if (this != &o) {
      cond_ = o.cond_;
      id_ = o.id_;
    }
    return *this;
  }
  const ContextCond &get_cond() const { return cond_; }
  ContextCond &get_cond() { return cond_; }
  uint32_t get_id() const { return id_; }
  bool operator<(const CxtVar &rhs) const {
    if (id_ != rhs.id_)
      return id_ < rhs.id_;
    return cond_ < rhs.cond_;
  }
  bool operator==(const CxtVar &rhs) const {
    return id_ == rhs.id_ && cond_ == rhs.cond_;
  }
  bool operator!=(const CxtVar &rhs) const { return !(*this == rhs); }

private:
  ContextCond cond_;
  uint32_t id_;
};

/// Set of context-sensitive vars (context-sensitive points-to set).
using CxtPtSet = std::set<CxtVar>;

/// Context- and flow-sensitive DP item: (cur, loc) + ContextCond.
template <class LocCond> class CxtStmtDPItem : public StmtDPItem<LocCond> {
public:
  using StmtDPItem<LocCond>::cur;
  using StmtDPItem<LocCond>::curloc;

  CxtStmtDPItem(const CxtVar &var, const LocCond *locCond)
      : StmtDPItem<LocCond>(var.get_id(), locCond), context_(var.get_cond()) {}
  CxtStmtDPItem(const CxtStmtDPItem &o)
      : StmtDPItem<LocCond>(o), context_(o.context_) {}
  CxtStmtDPItem &operator=(const CxtStmtDPItem &o) {
    if (this != &o) {
      StmtDPItem<LocCond>::operator=(o);
      context_ = o.context_;
    }
    return *this;
  }

  CxtVar getCondVar() const { return CxtVar(context_, this->cur); }
  const ContextCond &getCond() const { return context_; }
  ContextCond &getCond() { return context_; }
  bool pushContext(uint32_t cxt) { return context_.pushContext(cxt); }
  bool matchContext(uint32_t cxt) { return context_.matchContext(cxt); }

  bool operator<(const CxtStmtDPItem &rhs) const {
    if (this->cur != rhs.cur)
      return this->cur < rhs.cur;
    if (this->curloc != rhs.curloc)
      return this->curloc < rhs.curloc;
    return context_ < rhs.context_;
  }
  bool operator==(const CxtStmtDPItem &rhs) const {
    return this->cur == rhs.cur && this->curloc == rhs.curloc &&
           context_ == rhs.context_;
  }
  bool operator!=(const CxtStmtDPItem &rhs) const { return !(*this == rhs); }

  /// Debug dump (SVF-style): cur, location, and call-string context.
  void dump(llvm::raw_ostream &os) const {
    os << "cur=" << this->cur
       << " loc=" << static_cast<const void *>(this->curloc) << " "
       << context_.toString();
  }

private:
  ContextCond context_;
};

using CxtLocDPItem = CxtStmtDPItem<SVFGNode>;

} // namespace analysis
} // namespace lotus

#ifndef ASER_PTA_BDDPTS_H
#define ASER_PTA_BDDPTS_H

#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/PTSTrait.h"
#include "Alias/PtsSet/BDDPtsSet.h"

#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorHandling.h>

namespace aser {

extern llvm::cl::opt<bool> ConfigUseBDDPts;
extern llvm::cl::opt<bool> ConfigBDDPtsReorder;
extern llvm::cl::opt<std::string> ConfigBDDPtsReorderMethod;

// BDDAndersPtsSet backed points-to set implementation for AserPTA.
class BDDPts {
private:
  using TargetID = NodeID;

  class VariantSet {
  public:
    using iterator = BDDAndersPtsSet::iterator;

    VariantSet() = default;
    VariantSet(const VariantSet &) = default;
    VariantSet &operator=(const VariantSet &) = default;
    VariantSet(VariantSet &&) noexcept = default;
    VariantSet &operator=(VariantSet &&) noexcept = default;
    ~VariantSet() = default;

    bool has(TargetID idx) const;
    bool insert(TargetID idx);
    bool contains(const VariantSet &other) const;
    bool intersectWith(const VariantSet &other) const;
    bool unionWith(const VariantSet &other);
    void clear();
    size_t count() const;
    bool isEmpty() const;
    bool empty() const {
      return isEmpty();
    } // Alias for SparseBitVector compatibility
    bool equals(const VariantSet &other) const;

    // Compute: this = lhs \ rhs (elements in lhs but not in rhs)
    bool intersectWithComplement(const VariantSet &lhs, const VariantSet &rhs);

    // Union assignment operator for compatibility (returns true if changed)
    bool operator|=(const VariantSet &other);

    iterator begin() const;
    iterator end() const;

  private:
    BDDAndersPtsSet set;
  };

public:
  using PtsTy = VariantSet;
  using iterator = PtsTy::iterator;

  static inline void onNewNodeCreation(NodeID id) {
    assert(id == ptsVec.size());
    ptsVec.emplace_back();
    assert(ptsVec.size() == id + 1);
  }

  static inline void clearAll() { ptsVec.clear(); }

  [[nodiscard]] static inline const PtsTy &getPointsTo(NodeID id) {
    validateId(id);
    return ptsVec[id];
  }

  static inline bool unionWith(NodeID src, NodeID dst) {
    validateId(src);
    validateId(dst);
    return ptsVec[src].unionWith(ptsVec[dst]);
  }

  [[nodiscard]] static inline bool intersectWith(NodeID src, NodeID dst) {
    validateId(src);
    validateId(dst);
    return ptsVec[src].intersectWith(ptsVec[dst]);
  }

  [[nodiscard]] static inline bool intersectWithNoSpecialNode(NodeID src,
                                                              NodeID dst) {
    validateId(src);
    validateId(dst);
    const auto &lhs = ptsVec[src];
    const auto &rhs = ptsVec[dst];
    if (lhs.isEmpty() || rhs.isEmpty())
      return false;
    for (unsigned long lh : lhs) {
      if (lh < NORMAL_NODE_START_ID)
        continue;
      if (rhs.has(lh))
        return true;
    }
    return false;
  }

  static inline bool insert(NodeID src, TargetID idx) {
    validateId(src);
    return ptsVec[src].insert(idx);
  }

  [[nodiscard]] static inline bool has(NodeID src, TargetID idx) {
    validateId(src);
    return ptsVec[src].has(idx);
  }

  [[nodiscard]] static inline bool equal(NodeID src, NodeID dst) {
    validateId(src);
    validateId(dst);
    return ptsVec[src].equals(ptsVec[dst]);
  }

  [[nodiscard]] static inline bool contains(NodeID src, NodeID dst) {
    validateId(src);
    validateId(dst);
    return ptsVec[src].contains(ptsVec[dst]);
  }

  [[nodiscard]] static inline bool isEmpty(NodeID id) {
    validateId(id);
    return ptsVec[id].isEmpty();
  }

  [[nodiscard]] static inline iterator begin(NodeID id) {
    validateId(id);
    return ptsVec[id].begin();
  }

  [[nodiscard]] static inline iterator end(NodeID id) {
    validateId(id);
    return ptsVec[id].end();
  }

  static inline void clear(NodeID id) {
    validateId(id);
    ptsVec[id].clear();
  }

  static inline size_t count(NodeID id) {
    validateId(id);
    return ptsVec[id].count();
  }

  static inline const PtsTy &getPointedBy(NodeID) {
    llvm_unreachable(
        "pointed-by is not supported by ConfigurablePTS; use PointedByPts");
  }

  static inline constexpr bool supportPointedBy() { return false; }

private:
  static inline void validateId(NodeID id) { assert(id < ptsVec.size()); }

  static std::vector<PtsTy> ptsVec;

  friend struct PTSTrait<BDDPts>;
};

} // namespace aser

// === Inline implementation ============================================= //

namespace aser {

inline bool BDDPts::VariantSet::has(TargetID idx) const {
  return set.has(static_cast<BDDAndersPtsSet::Index>(idx));
}

inline bool BDDPts::VariantSet::insert(TargetID idx) {
  return set.insert(static_cast<BDDAndersPtsSet::Index>(idx));
}

inline bool BDDPts::VariantSet::contains(const VariantSet &other) const {
  return set.contains(other.set);
}

inline bool BDDPts::VariantSet::intersectWith(const VariantSet &other) const {
  return set.intersectWith(other.set);
}

inline bool BDDPts::VariantSet::unionWith(const VariantSet &other) {
  return set.unionWith(other.set);
}

inline void BDDPts::VariantSet::clear() { set.clear(); }

inline size_t BDDPts::VariantSet::count() const { return set.getSize(); }

inline bool BDDPts::VariantSet::isEmpty() const { return set.isEmpty(); }

inline bool BDDPts::VariantSet::equals(const VariantSet &other) const {
  return set == other.set;
}

inline BDDPts::VariantSet::iterator BDDPts::VariantSet::begin() const {
  return set.begin();
}

inline BDDPts::VariantSet::iterator BDDPts::VariantSet::end() const {
  return set.end();
}

inline bool BDDPts::VariantSet::intersectWithComplement(const VariantSet &lhs,
                                                        const VariantSet &rhs) {
  // Compute: this = lhs \ rhs (elements in lhs but not in rhs)
  set.clear();
  bool changed = false;
  for (auto it = lhs.begin(), ie = lhs.end(); it != ie; ++it) {
    if (!rhs.has(static_cast<TargetID>(*it))) {
      set.insert(*it);
      changed = true;
    }
  }
  return changed;
}

inline bool BDDPts::VariantSet::operator|=(const VariantSet &other) {
  return unionWith(other);
}

} // namespace aser

DEFINE_PTS_TRAIT(BDDPts)

#endif

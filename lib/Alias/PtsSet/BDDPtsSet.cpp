// A real BDD-backed points-to set built on CUDD.
#include "Alias/PtsSet/BDDPtsSet.h"

#include "Solvers/CUDD/cudd.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Use a fixed-width binary encoding for element indices.
using Index = BDDAndersPtsSet::Index;
constexpr unsigned kIndexBits = sizeof(Index) * 8;

std::atomic<bool> reorderEnabled{false};
std::atomic<BDDAndersPtsSet::ReorderingMethod> reorderMethod{
    BDDAndersPtsSet::ReorderingMethod::Sift};

DdManager *&managerRef() {
  static DdManager *manager = nullptr;
  return manager;
}

Cudd_ReorderingType toCuddReordering(BDDAndersPtsSet::ReorderingMethod method) {
  switch (method) {
  case BDDAndersPtsSet::ReorderingMethod::Sift:
    return CUDD_REORDER_SIFT;
  case BDDAndersPtsSet::ReorderingMethod::SiftConverge:
    return CUDD_REORDER_SIFT_CONVERGE;
  case BDDAndersPtsSet::ReorderingMethod::SymmSift:
    return CUDD_REORDER_SYMM_SIFT;
  case BDDAndersPtsSet::ReorderingMethod::SymmSiftConverge:
    return CUDD_REORDER_SYMM_SIFT_CONV;
  case BDDAndersPtsSet::ReorderingMethod::GroupSift:
    return CUDD_REORDER_GROUP_SIFT;
  case BDDAndersPtsSet::ReorderingMethod::GroupSiftConverge:
    return CUDD_REORDER_GROUP_SIFT_CONV;
  case BDDAndersPtsSet::ReorderingMethod::Window2:
    return CUDD_REORDER_WINDOW2;
  case BDDAndersPtsSet::ReorderingMethod::Window3:
    return CUDD_REORDER_WINDOW3;
  case BDDAndersPtsSet::ReorderingMethod::Window4:
    return CUDD_REORDER_WINDOW4;
  case BDDAndersPtsSet::ReorderingMethod::Window2Converge:
    return CUDD_REORDER_WINDOW2_CONV;
  case BDDAndersPtsSet::ReorderingMethod::Window3Converge:
    return CUDD_REORDER_WINDOW3_CONV;
  case BDDAndersPtsSet::ReorderingMethod::Window4Converge:
    return CUDD_REORDER_WINDOW4_CONV;
  case BDDAndersPtsSet::ReorderingMethod::Random:
    return CUDD_REORDER_RANDOM;
  case BDDAndersPtsSet::ReorderingMethod::RandomPivot:
    return CUDD_REORDER_RANDOM_PIVOT;
  case BDDAndersPtsSet::ReorderingMethod::Annealing:
    return CUDD_REORDER_ANNEALING;
  case BDDAndersPtsSet::ReorderingMethod::Genetic:
    return CUDD_REORDER_GENETIC;
  case BDDAndersPtsSet::ReorderingMethod::Linear:
    return CUDD_REORDER_LINEAR;
  case BDDAndersPtsSet::ReorderingMethod::LinearConverge:
    return CUDD_REORDER_LINEAR_CONVERGE;
  case BDDAndersPtsSet::ReorderingMethod::LazySift:
    return CUDD_REORDER_LAZY_SIFT;
  case BDDAndersPtsSet::ReorderingMethod::Exact:
    return CUDD_REORDER_EXACT;
  }
  return CUDD_REORDER_SIFT;
}

void applyReorderingConfig(DdManager *mgr) {
  if (reorderEnabled.load(std::memory_order_relaxed)) {
    Cudd_AutodynEnable(
        mgr, toCuddReordering(reorderMethod.load(std::memory_order_relaxed)));
  } else {
    Cudd_AutodynDisable(mgr);
  }
}

DdManager *getManager() {
  static std::once_flag initFlag;
  std::call_once(initFlag, []() {
    auto &manager = managerRef();
    manager = Cudd_Init(kIndexBits, 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
    applyReorderingConfig(manager);
  });
  return managerRef();
}

std::vector<DdNode *> &cubeCache() {
  static std::vector<DdNode *> cubes;
  return cubes;
}

// Build (and cache) a cube that encodes a specific element index.
DdNode *getCube(Index idx) {
  auto &cubes = cubeCache();
  if (idx < cubes.size() && cubes[idx])
    return cubes[idx];

  if (idx >= cubes.size())
    cubes.resize(idx + 1, nullptr);

  DdManager *mgr = getManager();
  DdNode *cube = Cudd_ReadOne(mgr);
  Cudd_Ref(cube);
  for (unsigned bit = 0; bit < kIndexBits; ++bit) {
    const Index mask = Index{1} << bit;
    DdNode *var = Cudd_bddIthVar(mgr, bit);
    DdNode *lit = (idx & mask) ? var : Cudd_Not(var); // NOLINT
    DdNode *tmp = Cudd_bddAnd(mgr, cube, lit);
    Cudd_Ref(tmp);
    Cudd_RecursiveDeref(mgr, cube);
    cube = tmp;
  }

  cubes[idx] = cube;
  return cube;
}

inline DdNode *logicZero() { return Cudd_ReadLogicZero(getManager()); }

std::string normalizeReorderName(const std::string &name) {
  std::string key;
  key.reserve(name.size());
  for (unsigned char c : name) {
    if (c == '_')
      c = '-';
    key.push_back(static_cast<char>(std::tolower(c)));
  }
  return key;
}

} // namespace

struct BDDAndersPtsSet::Impl {
  explicit Impl(DdNode *root) : bdd(root) { Cudd_Ref(bdd); }
  Impl() : Impl(logicZero()) {}
  ~Impl() { Cudd_RecursiveDeref(getManager(), bdd); }

  // Non-copyable: copying is handled at BDDAndersPtsSet level
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;

  DdNode *bdd;
};

void BDDAndersPtsSet::configureReordering(bool enable,
                                          ReorderingMethod method) {
  reorderEnabled.store(enable, std::memory_order_relaxed);
  reorderMethod.store(method, std::memory_order_relaxed);
  if (auto *mgr = managerRef())
    applyReorderingConfig(mgr);
}

bool BDDAndersPtsSet::parseReorderingMethod(const std::string &name,
                                            ReorderingMethod &out) {
  const std::string key = normalizeReorderName(name);
  if (key == "sift") {
    out = ReorderingMethod::Sift;
    return true;
  }
  if (key == "sift-conv" || key == "sift-converge") {
    out = ReorderingMethod::SiftConverge;
    return true;
  }
  if (key == "symm-sift") {
    out = ReorderingMethod::SymmSift;
    return true;
  }
  if (key == "symm-sift-conv" || key == "symm-sift-converge") {
    out = ReorderingMethod::SymmSiftConverge;
    return true;
  }
  if (key == "group-sift") {
    out = ReorderingMethod::GroupSift;
    return true;
  }
  if (key == "group-sift-conv" || key == "group-sift-converge") {
    out = ReorderingMethod::GroupSiftConverge;
    return true;
  }
  if (key == "window2") {
    out = ReorderingMethod::Window2;
    return true;
  }
  if (key == "window3") {
    out = ReorderingMethod::Window3;
    return true;
  }
  if (key == "window4") {
    out = ReorderingMethod::Window4;
    return true;
  }
  if (key == "window2-conv" || key == "window2-converge") {
    out = ReorderingMethod::Window2Converge;
    return true;
  }
  if (key == "window3-conv" || key == "window3-converge") {
    out = ReorderingMethod::Window3Converge;
    return true;
  }
  if (key == "window4-conv" || key == "window4-converge") {
    out = ReorderingMethod::Window4Converge;
    return true;
  }
  if (key == "random") {
    out = ReorderingMethod::Random;
    return true;
  }
  if (key == "random-pivot") {
    out = ReorderingMethod::RandomPivot;
    return true;
  }
  if (key == "annealing") {
    out = ReorderingMethod::Annealing;
    return true;
  }
  if (key == "genetic") {
    out = ReorderingMethod::Genetic;
    return true;
  }
  if (key == "linear") {
    out = ReorderingMethod::Linear;
    return true;
  }
  if (key == "linear-conv" || key == "linear-converge") {
    out = ReorderingMethod::LinearConverge;
    return true;
  }
  if (key == "lazy-sift") {
    out = ReorderingMethod::LazySift;
    return true;
  }
  if (key == "exact") {
    out = ReorderingMethod::Exact;
    return true;
  }
  return false;
}

BDDAndersPtsSet::BDDAndersPtsSet() : impl(new Impl()) {}

BDDAndersPtsSet::BDDAndersPtsSet(const BDDAndersPtsSet &other)
    : impl(new Impl(other.impl->bdd)) {}

BDDAndersPtsSet::BDDAndersPtsSet(BDDAndersPtsSet &&other) noexcept
    : impl(std::move(other.impl)), cache(std::move(other.cache)) {}

BDDAndersPtsSet &BDDAndersPtsSet::operator=(const BDDAndersPtsSet &other) {
  if (this == &other)
    return *this;
  impl = std::make_unique<Impl>(other.impl->bdd);
  cache.reset();
  return *this;
}

BDDAndersPtsSet &BDDAndersPtsSet::operator=(BDDAndersPtsSet &&other) noexcept {
  if (this == &other)
    return *this;
  impl = std::move(other.impl);
  cache = std::move(other.cache);
  return *this;
}

BDDAndersPtsSet::~BDDAndersPtsSet() = default;

bool BDDAndersPtsSet::has(Index idx) {
  return static_cast<const BDDAndersPtsSet &>(*this).has(idx);
}

bool BDDAndersPtsSet::has(Index idx) const {
  return Cudd_bddLeq(getManager(), getCube(idx), impl->bdd);
}

bool BDDAndersPtsSet::insert(Index idx) {
  DdManager *mgr = getManager();
  DdNode *cube = getCube(idx);
  if (Cudd_bddLeq(mgr, cube, impl->bdd))
    return false;

  DdNode *merged = Cudd_bddOr(mgr, impl->bdd, cube);
  Cudd_Ref(merged);
  Cudd_RecursiveDeref(mgr, impl->bdd);
  impl->bdd = merged;
  cache.reset();
  return true;
}

bool BDDAndersPtsSet::contains(const BDDAndersPtsSet &other) const {
  return Cudd_bddLeq(getManager(), other.impl->bdd, impl->bdd);
}

bool BDDAndersPtsSet::intersectWith(const BDDAndersPtsSet &other) const {
  DdManager *mgr = getManager();
  DdNode *intersection = Cudd_bddAnd(mgr, impl->bdd, other.impl->bdd);
  Cudd_Ref(intersection);
  const bool nonEmpty = intersection != logicZero();
  Cudd_RecursiveDeref(mgr, intersection);
  return nonEmpty;
}

bool BDDAndersPtsSet::unionWith(const BDDAndersPtsSet &other) {
  DdManager *mgr = getManager();
  DdNode *merged = Cudd_bddOr(mgr, impl->bdd, other.impl->bdd);
  Cudd_Ref(merged);
  const bool changed = merged != impl->bdd;
  Cudd_RecursiveDeref(mgr, impl->bdd);
  impl->bdd = merged;
  if (changed)
    cache.reset();
  return changed;
}

bool BDDAndersPtsSet::differenceWith(const BDDAndersPtsSet &other) {
  DdManager *mgr = getManager();
  DdNode *diff = Cudd_bddAnd(mgr, impl->bdd, Cudd_Not(other.impl->bdd));
  Cudd_Ref(diff);
  const bool changed = diff != impl->bdd;
  Cudd_RecursiveDeref(mgr, impl->bdd);
  impl->bdd = diff;
  if (changed)
    cache.reset();
  return changed;
}

bool BDDAndersPtsSet::complement() {
  DdManager *mgr = getManager();
  DdNode *neg = Cudd_Not(impl->bdd);
  Cudd_Ref(neg);
  const bool changed = neg != impl->bdd;
  Cudd_RecursiveDeref(mgr, impl->bdd);
  impl->bdd = neg;
  if (changed)
    cache.reset();
  return changed;
}

void BDDAndersPtsSet::clear() {
  DdManager *mgr = getManager();
  if (impl->bdd == logicZero())
    return;
  Cudd_RecursiveDeref(mgr, impl->bdd);
  impl->bdd = logicZero();
  Cudd_Ref(impl->bdd);
  cache.reset();
}

unsigned BDDAndersPtsSet::getSize() const {
  const double count = Cudd_CountMinterm(getManager(), impl->bdd, kIndexBits);
  return static_cast<unsigned>(std::lround(count));
}

bool BDDAndersPtsSet::isEmpty() const { return impl->bdd == logicZero(); }

bool BDDAndersPtsSet::operator==(const BDDAndersPtsSet &other) const {
  return impl->bdd == other.impl->bdd;
}

void BDDAndersPtsSet::refreshCache() const {
  if (cache)
    return;

  auto elems = std::make_shared<std::vector<Index>>();
  if (isEmpty()) {
    cache = elems;
    return;
  }

  DdGen *gen;
  int *cube;
  CUDD_VALUE_TYPE value;
  Cudd_ForeachCube(getManager(), impl->bdd, gen, cube, value) {
    // Build a base index from bits fixed to 0/1 and record positions of
    // "don't-care" bits (encoded as 2 in CUDD).
    Index base = 0;
    unsigned dcCount = 0;
    unsigned
        dcPositions[kIndexBits]; // kIndexBits is small (≤64), stack-allocate.

    for (unsigned bit = 0; bit < kIndexBits; ++bit) {
      if (cube[bit] == 0) {
        // fixed to 0 – nothing to do
      } else if (cube[bit] == 1) {
        base |= (Index{1} << bit);
      } else { // cube[bit] == 2  → don't-care
        dcPositions[dcCount++] = bit;
      }
    }

    const Index combos = Index{1} << dcCount;
    for (Index m = 0; m < combos; ++m) {
      Index idx = base;
      for (unsigned i = 0; i < dcCount; ++i) {
        if (m & (Index{1} << i))
          idx |= (Index{1} << dcPositions[i]);
        else
          idx &= ~(Index{1} << dcPositions[i]); // ensure 0 when bit not set
      }
      elems->push_back(idx);
    }
  }

  cache = elems;
}

BDDAndersPtsSet::iterator BDDAndersPtsSet::begin() const {
  refreshCache();
  return cache->begin();
}

BDDAndersPtsSet::iterator BDDAndersPtsSet::end() const {
  refreshCache();
  return cache->end();
}

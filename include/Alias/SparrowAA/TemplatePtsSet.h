/**
 * @file TemplatePtsSet.h
 * @brief Runtime-selectable points-to set for Andersen's analysis.
 *
 * ## Design
 *
 * `RuntimePtsSet` is a **type-erased wrapper** around the concrete points-to
 * set implementations (`AndersPtsSet` backed by `llvm::SparseBitVector`, or
 * `BDDAndersPtsSet` backed by a BDD library).  It uses the **type-erasure
 * idiom** (Concept/Model pattern) to allow the implementation to be selected
 * at runtime via `selectImplementation()` without changing any call sites.
 *
 * ## Implementation Selection
 *
 * The active implementation is stored in a process-global variable and
 * applies to all `RuntimePtsSet` objects created after the call:
 *
 * ```cpp
 * // Select BDD backend before constructing the Andersen object:
 * selectGlobalPtsSetImpl(PtsSetImpl::BDD);
 * Andersen aa(M);
 * ```
 *
 * The default is `PtsSetImpl::SPARSE_BITVECTOR`.
 *
 * ## Type Erasure (Concept/Model)
 *
 * Internally, `RuntimePtsSet` holds a `std::unique_ptr<Concept>` where
 * `Concept` is a pure-virtual interface.  `Model<Impl>` is a concrete
 * template that wraps a specific `Impl` (e.g., `AndersPtsSet`).  This
 * avoids virtual dispatch in the common case by using `dynamic_cast` to
 * detect same-type operands and fall back to the virtual path only for
 * cross-type operations (which should not occur in practice).
 *
 * ## Iteration Cache
 *
 * Because `llvm::SparseBitVector::iterator` is not a random-access iterator,
 * `RuntimePtsSet` maintains a lazily-populated `std::vector<Index>` cache
 * for `begin()`/`end()`.  The cache is invalidated on any mutating operation
 * (`insert`, `unionWith`, `clear`).
 *
 * ## Aliases
 *
 * `DefaultPtsSet` is a type alias for `RuntimePtsSet` and is used throughout
 * the Andersen implementation.
 */

#ifndef ANDERSEN_TEMPLATE_PTSSET_H
#define ANDERSEN_TEMPLATE_PTSSET_H

#include "Alias/PtsSet/BDDPtsSet.h"
#include "Alias/SparrowAA/PtsSet.h"

#include <memory>

/**
 * @enum PtsSetImpl
 * @brief Selects the backing data structure for `RuntimePtsSet`.
 */
enum class PtsSetImpl {
  SPARSE_BITVECTOR, ///< Use `llvm::SparseBitVector` (default; good for sparse
                    ///< sets).
  BDD               ///< Use a BDD library (better for large, dense sets).
};

/**
 * @class RuntimePtsSet
 * @brief Type-erased, runtime-selectable points-to set.
 *
 * Presents the same public interface as `AndersPtsSet` but delegates all
 * operations to the implementation selected by `selectImplementation()`.
 *
 * @note All mutating operations (`insert`, `unionWith`, `clear`) invalidate
 *       the iteration cache.  Iterating over the set materialises the cache.
 */
class RuntimePtsSet {
public:
  using Index = std::uint64_t;
  using iterator = std::vector<Index>::const_iterator;

  RuntimePtsSet();
  RuntimePtsSet(const RuntimePtsSet &);
  RuntimePtsSet(RuntimePtsSet &&) noexcept = default;
  RuntimePtsSet &operator=(const RuntimePtsSet &);
  RuntimePtsSet &operator=(RuntimePtsSet &&) noexcept = default;
  ~RuntimePtsSet() = default;

  bool has(Index idx);
  bool has(Index idx) const;
  bool insert(Index idx);
  bool contains(const RuntimePtsSet &other) const;
  bool intersectWith(const RuntimePtsSet &other) const;
  bool unionWith(const RuntimePtsSet &other);

  void clear();
  unsigned getSize() const;
  bool isEmpty() const;
  bool operator==(const RuntimePtsSet &other) const;

  iterator begin() const;
  iterator end() const;

  static void selectImplementation(PtsSetImpl impl);
  static PtsSetImpl selectedImplementation();

private:
  struct Concept {
    virtual ~Concept() = default;
    virtual bool has(Index) const = 0;
    virtual bool insert(Index) = 0;
    virtual bool contains(const Concept &) const = 0;
    virtual bool intersectWith(const Concept &) const = 0;
    virtual bool unionWith(const Concept &) = 0;
    virtual void clear() = 0;
    virtual unsigned getSize() const = 0;
    virtual bool isEmpty() const = 0;
    virtual bool equals(const Concept &) const = 0;
    virtual std::unique_ptr<Concept> clone() const = 0;
    virtual void materialize(std::vector<Index> &out) const = 0;
  };

  template <typename Impl> struct Model : Concept {
    Impl set;

    bool has(Index idx) const override {
      return set.has(static_cast<unsigned>(idx));
    }
    bool insert(Index idx) override {
      return set.insert(static_cast<unsigned>(idx));
    }

    bool contains(const Concept &other) const override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set.contains(same->set);
      std::vector<Index> tmp;
      other.materialize(tmp);
      for (Index v : tmp)
        if (!set.has(static_cast<unsigned>(v)))
          return false;
      return true;
    }

    bool intersectWith(const Concept &other) const override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set.intersectWith(same->set);
      std::vector<Index> tmp;
      other.materialize(tmp);
      for (Index v : tmp)
        if (set.has(static_cast<unsigned>(v)))
          return true;
      return false;
    }

    bool unionWith(const Concept &other) override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set.unionWith(same->set);
      std::vector<Index> tmp;
      other.materialize(tmp);
      bool changed = false;
      for (Index v : tmp)
        changed |= set.insert(static_cast<unsigned>(v));
      return changed;
    }

    void clear() override { set.clear(); }
    unsigned getSize() const override { return set.getSize(); }
    bool isEmpty() const override { return set.isEmpty(); }

    bool equals(const Concept &other) const override {
      if (auto *same = dynamic_cast<const Model *>(&other))
        return set == same->set;
      std::vector<Index> lhs;
      std::vector<Index> rhs;
      materialize(lhs);
      other.materialize(rhs);
      return lhs == rhs;
    }

    std::unique_ptr<Concept> clone() const override {
      return std::make_unique<Model>(*this);
    }

    void materialize(std::vector<Index> &out) const override {
      for (auto it = set.begin(), ie = set.end(); it != ie; ++it)
        out.push_back(static_cast<Index>(*it));
    }
  };

  /// @brief Construct a new `Concept` instance for the currently active
  /// implementation.
  static std::unique_ptr<Concept> makeImpl();
  /// @brief Populate `cache` from `impl` if the cache is stale.
  void refreshCache() const;

  std::unique_ptr<Concept> impl; ///< Type-erased concrete implementation.
  /// Lazily-populated sorted element cache for iteration.
  /// Shared across copies (copy-on-write semantics via `shared_ptr`).
  /// Reset to nullptr on any mutating operation.
  mutable std::shared_ptr<std::vector<Index>> cache;

  /// @brief Process-global implementation selector (default: SPARSE_BITVECTOR).
  static PtsSetImpl &activeImpl();
};

/**
 * @brief Set the global points-to set implementation for all future
 * `RuntimePtsSet` objects.
 * @param impl  The desired implementation (`SPARSE_BITVECTOR` or `BDD`).
 */
inline void selectGlobalPtsSetImpl(PtsSetImpl impl) {
  RuntimePtsSet::selectImplementation(impl);
}

/// @brief Return the currently active global points-to set implementation.
inline PtsSetImpl getGlobalPtsSetImpl() {
  return RuntimePtsSet::selectedImplementation();
}

/// @brief Alias for `RuntimePtsSet` — the name used throughout the Andersen
/// implementation.
using DefaultPtsSet = RuntimePtsSet;

// === Inline implementation details ===================================== //

inline std::unique_ptr<RuntimePtsSet::Concept> RuntimePtsSet::makeImpl() {
  if (activeImpl() == PtsSetImpl::BDD)
    return std::make_unique<Model<BDDAndersPtsSet>>();
  return std::make_unique<Model<AndersPtsSet>>();
}

inline RuntimePtsSet::RuntimePtsSet() : impl(makeImpl()) {}

inline RuntimePtsSet::RuntimePtsSet(const RuntimePtsSet &other)
    : impl(other.impl->clone()) {
  // B10 Fix: do NOT share the cache shared_ptr with the source object.
  // The original code did `cache(other.cache)`, which means both objects
  // pointed to the same std::vector.  If either object was subsequently
  // mutated (insert/unionWith/clear), it would call cache.reset() on its
  // own shared_ptr, leaving the other object's shared_ptr pointing to the
  // now-stale (but still live) vector.  The other object would then iterate
  // over stale cached values without realising the cache was invalid.
  //
  // Fix: leave cache as nullptr (default-constructed).  The cache will be
  // lazily rebuilt on the first call to begin()/end() after the copy.
  // This is slightly less efficient than sharing an immutable cache, but
  // it is correct.  A proper copy-on-write scheme would require additional
  // synchronisation and is not worth the complexity here.
}

inline RuntimePtsSet &RuntimePtsSet::operator=(const RuntimePtsSet &other) {
  if (this == &other)
    return *this;
  impl = other.impl->clone();
  // B10 Fix: same reasoning as the copy constructor — do not share the cache.
  cache.reset();
  return *this;
}

inline void RuntimePtsSet::selectImplementation(PtsSetImpl impl) {
  activeImpl() = impl;
}

inline PtsSetImpl RuntimePtsSet::selectedImplementation() {
  return activeImpl();
}

inline bool RuntimePtsSet::has(Index idx) {
  return static_cast<const RuntimePtsSet &>(*this).has(idx);
}

inline bool RuntimePtsSet::has(Index idx) const { return impl->has(idx); }

inline bool RuntimePtsSet::insert(Index idx) {
  cache.reset();
  return impl->insert(idx);
}

inline bool RuntimePtsSet::contains(const RuntimePtsSet &other) const {
  return impl->contains(*other.impl);
}

inline bool RuntimePtsSet::intersectWith(const RuntimePtsSet &other) const {
  return impl->intersectWith(*other.impl);
}

inline bool RuntimePtsSet::unionWith(const RuntimePtsSet &other) {
  cache.reset();
  return impl->unionWith(*other.impl);
}

inline void RuntimePtsSet::clear() {
  cache.reset();
  impl->clear();
}

inline unsigned RuntimePtsSet::getSize() const { return impl->getSize(); }

inline bool RuntimePtsSet::isEmpty() const { return impl->isEmpty(); }

inline bool RuntimePtsSet::operator==(const RuntimePtsSet &other) const {
  return impl->equals(*other.impl);
}

inline void RuntimePtsSet::refreshCache() const {
  if (cache)
    return;
  auto elems = std::make_shared<std::vector<Index>>();
  impl->materialize(*elems);
  cache = elems;
}

inline RuntimePtsSet::iterator RuntimePtsSet::begin() const {
  refreshCache();
  return cache->begin();
}

inline RuntimePtsSet::iterator RuntimePtsSet::end() const {
  refreshCache();
  return cache->end();
}

inline PtsSetImpl &RuntimePtsSet::activeImpl() {
  static PtsSetImpl impl = PtsSetImpl::SPARSE_BITVECTOR;
  return impl;
}

#endif // ANDERSEN_TEMPLATE_PTSSET_H
/*
 * BDD-backed points-to set using the CUDD package.
 *
 * This header intentionally hides all CUDD types behind a pimpl to avoid
 * leaking the heavy dependency into most translation units. The actual
 * implementation lives in lib/Alias/PtsSet/BDDPtsSet.cpp.
 */

#ifndef ANDERSEN_BDDPTSSET_H
#define ANDERSEN_BDDPTSSET_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class BDDAndersPtsSet {
public:
  enum class ReorderingMethod : std::uint8_t {
    Sift,
    SiftConverge,
    SymmSift,
    SymmSiftConverge,
    GroupSift,
    GroupSiftConverge,
    Window2,
    Window3,
    Window4,
    Window2Converge,
    Window3Converge,
    Window4Converge,
    Random,
    RandomPivot,
    Annealing,
    Genetic,
    Linear,
    LinearConverge,
    LazySift,
    Exact,
  };

  using Index = std::uint64_t;
  using iterator = std::vector<Index>::const_iterator;

  BDDAndersPtsSet();
  BDDAndersPtsSet(const BDDAndersPtsSet &);
  BDDAndersPtsSet(BDDAndersPtsSet &&) noexcept;
  BDDAndersPtsSet &operator=(const BDDAndersPtsSet &);
  BDDAndersPtsSet &operator=(BDDAndersPtsSet &&) noexcept;
  ~BDDAndersPtsSet();

  bool has(Index idx);
  bool has(Index idx) const;
  bool insert(Index idx);
  bool contains(const BDDAndersPtsSet &other) const;
  bool intersectWith(const BDDAndersPtsSet &other) const;
  bool unionWith(const BDDAndersPtsSet &other);
  bool differenceWith(const BDDAndersPtsSet &other);
  bool complement();

  void clear();
  unsigned getSize() const;
  bool isEmpty() const;
  bool operator==(const BDDAndersPtsSet &other) const;

  iterator begin() const;
  iterator end() const;

  static void
  configureReordering(bool enable,
                      ReorderingMethod method = ReorderingMethod::Sift);
  static bool parseReorderingMethod(const std::string &name,
                                    ReorderingMethod &out);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;

  // Materialize the BDD into a stable snapshot for iteration.
  void refreshCache() const;
  mutable std::shared_ptr<std::vector<Index>> cache;
};

#endif // ANDERSEN_BDDPTSSET_H

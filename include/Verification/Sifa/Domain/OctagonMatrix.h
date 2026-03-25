//===-- Verification/Sifa/Domain/OctagonMatrix.h --------------------------===//
//
// Octagon matrix (Ultimate OctagonMatrix-aligned). Miné's octagon domain:
// 2n×2n matrix for constraints ±vi ± vj ≤ c; indices 2i = +vi, 2i+1 = -vi.
// nullopt = +∞ (no constraint). strongClosure, max (join), widenSimple.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_OCTAGONMATRIX_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_OCTAGONMATRIX_H

#include "llvm/ADT/Optional.h"

#include <cstddef>
#include <limits>
#include <vector>

namespace lotus {
namespace sifa {

/// Octagon matrix: 2n×2n, entry (i,j) = c means constraint ≤ c; nullopt = +∞.
class OctagonMatrix {
public:
  explicit OctagonMatrix(std::size_t nVars = 0) : n_(nVars) {
    std::size_t dim = 2 * nVars;
    matrix_.assign(dim, std::vector<llvm::Optional<int64_t>>(dim, llvm::None));
  }

  std::size_t numVars() const { return n_; }
  std::size_t dim() const { return matrix_.size(); }

  llvm::Optional<int64_t> get(std::size_t i, std::size_t j) const {
    if (i >= matrix_.size() || j >= matrix_.size())
      return llvm::None;
    return matrix_[i][j];
  }
  void set(std::size_t i, std::size_t j, int64_t c) {
    if (i < matrix_.size() && j < matrix_.size())
      matrix_[i][j] = c;
  }

  /// Element-wise max (join: looser bound). Same size only.
  OctagonMatrix max(const OctagonMatrix &other) const {
    if (dim() != other.dim())
      return *this;
    OctagonMatrix out(n_);
    for (std::size_t i = 0; i < dim(); ++i)
      for (std::size_t j = 0; j < dim(); ++j) {
        auto a = get(i, j), b = other.get(i, j);
        if (a && b)
          out.matrix_[i][j] = std::max(*a, *b);
        else if (a)
          out.matrix_[i][j] = *a;
        else if (b)
          out.matrix_[i][j] = *b;
      }
    return out;
  }

  /// Widen: if other[i][j] > this[i][j] then unbounded (nullopt).
  OctagonMatrix widenSimple(const OctagonMatrix &other) const {
    if (dim() != other.dim())
      return *this;
    OctagonMatrix out(n_);
    for (std::size_t i = 0; i < dim(); ++i)
      for (std::size_t j = 0; j < dim(); ++j) {
        auto a = get(i, j), b = other.get(i, j);
        if (a && b && *b <= *a)
          out.matrix_[i][j] = *a;
        else if (a && (!b || *b <= *a))
          out.matrix_[i][j] = *a;
      }
    return out;
  }

  /// Strong closure (Floyd-Warshall). Entry nullopt = +∞.
  OctagonMatrix strongClosure() const {
    OctagonMatrix out = *this;
    const std::size_t d = dim();
    for (std::size_t k = 0; k < d; ++k)
      for (std::size_t i = 0; i < d; ++i)
        for (std::size_t j = 0; j < d; ++j) {
          auto ik = out.get(i, k), kj = out.get(k, j);
          if (!ik || !kj)
            continue;
          int64_t sum;
          if (__builtin_add_overflow(*ik, *kj, &sum))
            continue;
          auto ij = out.get(i, j);
          if (!ij || sum < *ij)
            out.set(i, j, sum);
        }
    return out;
  }

  /// True iff any diagonal entry < 0 (inconsistent).
  bool hasNegativeSelfLoop() const {
    for (std::size_t i = 0; i < dim(); ++i) {
      auto c = get(i, i);
      if (c && *c < 0)
        return true;
    }
    return false;
  }

  /// Relax variable \p varIdx: return new matrix with all constraints involving
  /// ±v set to +∞ (sound over-approximation for havoc).
  OctagonMatrix relaxVar(std::size_t varIdx) const {
    if (varIdx >= n_)
      return *this;
    OctagonMatrix out(n_);
    const std::size_t d = dim();
    const std::size_t lo = 2 * varIdx, hi = 2 * varIdx + 1;
    for (std::size_t i = 0; i < d; ++i)
      for (std::size_t j = 0; j < d; ++j) {
        if (i == lo || i == hi || j == lo || j == hi)
          continue;
        auto c = get(i, j);
        if (c)
          out.set(i, j, *c);
      }
    return out;
  }

  /// Rearrange to new variable order. \p copyInstructions[newIdx] = oldIdx (or
  /// dim() for unbounded).
  OctagonMatrix
  rearrange(const std::vector<std::size_t> &copyInstructions) const {
    const std::size_t d = copyInstructions.size();
    OctagonMatrix out(d / 2);
    if (out.dim() != d)
      return *this;
    for (std::size_t i = 0; i < d; ++i)
      for (std::size_t j = 0; j < d; ++j) {
        std::size_t si = copyInstructions[i], sj = copyInstructions[j];
        if (si < dim() && sj < dim()) {
          auto c = get(si, sj);
          if (c)
            out.set(i, j, *c);
        }
      }
    return out;
  }

private:
  std::size_t n_;
  std::vector<std::vector<llvm::Optional<int64_t>>> matrix_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_OCTAGONMATRIX_H

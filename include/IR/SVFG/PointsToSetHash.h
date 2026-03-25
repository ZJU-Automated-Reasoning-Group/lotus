//===- PointsToSetHash.h -- Efficient Points-to Set Hashing -----------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// PointsToSetHash: Efficient hashing and comparison for points-to sets.
//
// This replaces the string-based canonicalization in SVFGBuilder with
// a proper hash-based approach for O(1) lookup instead of O(n) string
// construction and comparison.
//
// Design:
// - Uses FNV-1a hash for fast, collision-resistant hashing
// - Compares by hash first, then by value for correctness
// - Reuses SVFGNodeBS (std::set<uint32_t>) directly
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGNode.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace lotus {
namespace analysis {

/// @brief Efficient hash for points-to sets
struct PointsToSetHash {
  /// FNV-1a constants
  static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
  static constexpr uint64_t FNV_PRIME = 1099511628211ULL;

  size_t operator()(const SVFGNodeBS &pts) const noexcept {
    if (pts.empty())
      return 0;

    uint64_t hash = FNV_OFFSET_BASIS;
    for (uint32_t id : pts) {
      // FNV-1a: hash = (hash XOR byte) * FNV_PRIME
      hash ^= static_cast<uint64_t>(id);
      hash *= FNV_PRIME;
    }
    return static_cast<size_t>(hash);
  }
};

/// @brief Efficient equality for points-to sets
struct PointsToSetEqual {
  bool operator()(const SVFGNodeBS &lhs, const SVFGNodeBS &rhs) const noexcept {
    // std::set already provides efficient comparison
    return lhs == rhs;
  }
};

/// @brief Wrapper for using SVFGNodeBS as unordered_map key
///
/// This avoids the overhead of string conversion while maintaining
/// the same canonical memory region semantics as the original implementation.
///
/// Usage:
///   std::unordered_map<SVFGNodeBS, uint32_t, PointsToSetHash, PointsToSetEqual>
///   ptsToMemReg;
class PointsToSetKey {
private:
  SVFGNodeBS pts;
  mutable size_t cachedHash = 0;
  mutable bool hashCached = false;

public:
  PointsToSetKey() = default;
  explicit PointsToSetKey(const SVFGNodeBS &p) : pts(p) {}
  explicit PointsToSetKey(SVFGNodeBS &&p) : pts(std::move(p)) {}

  const SVFGNodeBS &get() const { return pts; }

  size_t hash() const noexcept {
    if (!hashCached) {
      cachedHash = PointsToSetHash()(pts);
      hashCached = true;
    }
    return cachedHash;
  }

  bool operator==(const PointsToSetKey &other) const noexcept {
    // Fast path: compare cached hashes first
    if (hashCached && other.hashCached && cachedHash != other.cachedHash) {
      return false;
    }
    return pts == other.pts;
  }

  bool operator!=(const PointsToSetKey &other) const noexcept {
    return !(*this == other);
  }
};

} // namespace analysis
} // namespace lotus

/// @brief Specialize std::hash for PointsToSetKey
namespace std {
template <> struct hash<lotus::analysis::PointsToSetKey> {
  size_t operator()(const lotus::analysis::PointsToSetKey &key) const noexcept {
    return key.hash();
  }
};
} // namespace std

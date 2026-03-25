//===-- Verification/Sifa/Caches/StarDagCache.h ---------------------------===//
//
// Cache DAGs for the inner regex of stars (ported from Ultimate Sifa).
//
// Builds: compress(regexToDag(markRegex(inner, nullptr))).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CACHES_STARDAGCACHE_H
#define LOTUS_VERIFICATION_SIFA_CACHES_STARDAGCACHE_H

#include "Verification/Sifa/RegexDag/RegexDag.h"
#include "Verification/Sifa/RegexDag/RegexDagCompressor.h"
#include "Verification/Sifa/RegexDag/RegexDagUtils.h"
#include "Verification/Sifa/RegexDag/RegexToDag.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

#include <cstdint>
#include <unordered_map>

namespace lotus {
namespace sifa {

template <typename L> class StarDagCache final {
public:
  using RegexRef = lotus::pathexpressions::RegexRef<L>;
  using Dag = RegexDag<L>;

  explicit StarDagCache(SifaStats &stats) : stats_(stats) {}

  const Dag &dagOf(const RegexRef &regex) {
    const auto it = cache_.find(regex);
    if (it != cache_.end()) {
      return it->second;
    }
    return cache_.emplace(regex, computeDagOf(regex)).first->second;
  }

private:
  Dag computeDagOf(const RegexRef &regex);

  SifaStats &stats_;
  std::uint32_t nextMarkerId_ = 1;
  struct RegexRefKeyHash {
    std::size_t operator()(const RegexRef &r) const {
      return r ? r->hashCode() : 0;
    }
  };
  struct RegexRefKeyEq {
    bool operator()(const RegexRef &a, const RegexRef &b) const {
      if (a == b)
        return true;
      if (!a || !b)
        return false;
      return a->equals(*b);
    }
  };
  std::unordered_map<RegexRef, Dag, RegexRefKeyHash, RegexRefKeyEq> cache_;
};

template <typename L>
typename StarDagCache<L>::Dag
StarDagCache<L>::computeDagOf(const RegexRef &regex) {
  RegexToDag<L> r2d;
  const auto marked =
      markRegex(regex, /*finalLocationAsMark=*/nullptr, nextMarkerId_++);
  r2d.add(marked);
  Dag dag = r2d.getDagAndReset();
  RegexDagCompressor<L> comp;
  comp.compress(dag);
  (void)stats_;
  return dag;
}

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_CACHES_STARDAGCACHE_H

#pragma once

#include <set>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

struct LocksetDomain {
  using value_type = std::set<const llvm::Value *>;

  static value_type meet(const value_type &Lhs, const value_type &Rhs) {
    value_type Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  static bool equal(const value_type &Lhs, const value_type &Rhs) {
    return Lhs == Rhs;
  }

  static value_type meetIdentity() { return {}; }
};

using LocksetFact = LocksetDomain::value_type;

} // namespace elimination

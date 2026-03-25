/*
 * IFDS Sign Analysis
 *
 * Tracks sign facts (negative/zero/positive/unknown) for SSA values.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <llvm/ADT/Optional.h>

namespace ifds {

struct SignFact {
  enum class Kind { ZeroFact, ValueSign };
  enum class Sign { Negative, Zero, Positive, Unknown };

  Kind kind;
  const llvm::Value *value;
  Sign sign;

  static SignFact zero() { return {Kind::ZeroFact, nullptr, Sign::Unknown}; }
  static SignFact value_sign(const llvm::Value *v, Sign s) {
    return {Kind::ValueSign, v, s};
  }

  bool operator==(const SignFact &other) const {
    return kind == other.kind && value == other.value && sign == other.sign;
  }
  bool operator!=(const SignFact &other) const { return !(*this == other); }
  bool operator<(const SignFact &other) const {
    if (kind != other.kind) {
      return kind < other.kind;
    }
    if (value != other.value) {
      return std::less<const llvm::Value *>{}(value, other.value);
    }
    return sign < other.sign;
  }
};

} // namespace ifds

namespace std {
template <> struct hash<ifds::SignFact> {
  size_t operator()(const ifds::SignFact &fact) const {
    // FNV-1a-style mixing to avoid XOR-shift collisions on aligned hashes.
    size_t h = 14695981039346656037ULL;
    h ^= std::hash<int>{}(static_cast<int>(fact.kind));
    h *= 1099511628211ULL;
    h ^= std::hash<const llvm::Value *>{}(fact.value);
    h *= 1099511628211ULL;
    h ^= std::hash<int>{}(static_cast<int>(fact.sign));
    h *= 1099511628211ULL;
    return h;
  }
};
} // namespace std

namespace ifds {

class SignAnalysis : public DefaultNoAliasIFDSProblem<SignFact> {
public:
  SignFact zero_fact() const override { return SignFact::zero(); }
  FactSet normal_flow(const llvm::Instruction *stmt,
                      const llvm::Instruction *succ,
                      const SignFact &fact) override;
  FactSet call_flow(const llvm::CallBase *call, const llvm::Function *callee,
                    const SignFact &fact) override;
  FactSet return_flow(const llvm::CallBase *call, const llvm::Instruction *exit_inst, const llvm::Instruction *return_site, const llvm::Function *callee,
                      const SignFact &exit_fact,
                      const SignFact &call_fact) override;
  FactSet call_to_return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *return_site,
                              llvm::ArrayRef<const llvm::Function *> callees, const SignFact &fact) override;
  FactSet initial_facts(const llvm::Function *main) override;

private:
  static llvm::Optional<int64_t> as_const_i64(const llvm::Value *v);
  static SignFact::Sign sign_of(int64_t v);
  static llvm::Optional<SignFact::Sign> eval_binary(unsigned opcode, int64_t a,
                                                    int64_t b);
};

} // namespace ifds

//===-- Verification/Sifa/Domain/OctagonDomain.h
//---------------------------===//
//
// Octagon domain (ported from Ultimate Library-Sifa). Miné's octagon:
// ±vi ± vj ≤ c; OctagonState holds varToIndex + OctagonMatrix; join = max,
// widen = widenSimple, isBottom = strongClosure then hasNegativeSelfLoop.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_OCTAGONDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_OCTAGONDOMAIN_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Domain/IntervalDomain.h"
#include "Verification/Sifa/Domain/OctagonMatrix.h"

#include <unordered_map>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace lotus {
class AliasAnalysisWrapper;
namespace sifa {

/// Octagon state: varToIndex (Value* -> block index), matrix
/// (Ultimate-aligned).
class OctagonState {
public:
  OctagonState() = default;
  explicit OctagonState(bool isBottom) : isBottom_(isBottom) {}

  /// Build state from explicit varToIndex and matrix (for block transfer).
  OctagonState(std::unordered_map<const llvm::Value *, std::size_t> varToIndex,
               OctagonMatrix matrix, bool isBottom = false)
      : isBottom_(isBottom), varToIndex_(std::move(varToIndex)),
        matrix_(std::move(matrix)) {}

  bool isBottom() const { return isBottom_; }
  void setBottom(bool b) { isBottom_ = b; }

  const std::unordered_map<const llvm::Value *, std::size_t> &
  varToIndex() const {
    return varToIndex_;
  }
  const OctagonMatrix &matrix() const { return matrix_; }

  llvm::Optional<Interval> getMemory(const llvm::Value *region) const {
    auto it = memory_.find(region);
    if (it == memory_.end())
      return llvm::None;
    return it->second;
  }
  void setMemory(const llvm::Value *region, Interval i) {
    memory_[region] = std::move(i);
  }
  const std::unordered_map<const llvm::Value *, Interval> &memory() const {
    return memory_;
  }

  OctagonState join(const OctagonState &other) const {
    if (isBottom_)
      return other;
    if (other.isBottom_)
      return *this;
    std::vector<const llvm::Value *> allVars;
    for (const auto &p : varToIndex_)
      allVars.push_back(p.first);
    for (const auto &p : other.varToIndex_) {
      if (varToIndex_.count(p.first))
        continue;
      allVars.push_back(p.first);
    }
    const std::size_t n = allVars.size();
    std::unordered_map<const llvm::Value *, std::size_t> newVarToIndex;
    for (std::size_t i = 0; i < n; ++i)
      newVarToIndex[allVars[i]] = i;

    std::vector<std::size_t> copy1(2 * n, 2 * n), copy2(2 * n, 2 * n);
    for (std::size_t i = 0; i < n; ++i) {
      auto it1 = varToIndex_.find(allVars[i]);
      auto it2 = other.varToIndex_.find(allVars[i]);
      if (it1 != varToIndex_.end()) {
        copy1[2 * i] = 2 * it1->second;
        copy1[2 * i + 1] = 2 * it1->second + 1;
      }
      if (it2 != other.varToIndex_.end()) {
        copy2[2 * i] = 2 * it2->second;
        copy2[2 * i + 1] = 2 * it2->second + 1;
      }
    }
    OctagonMatrix m1 = (matrix_.dim() == 2 * n)
                           ? matrix_.rearrange(copy1)
                           : expandAndRearrange(matrix_, copy1, n);
    OctagonMatrix m2 = (other.matrix_.dim() == 2 * n)
                           ? other.matrix_.rearrange(copy2)
                           : expandAndRearrange(other.matrix_, copy2, n);
    OctagonState out;
    out.varToIndex_ = newVarToIndex;
    out.matrix_ = m1.max(m2);
    for (const auto &kv : memory_)
      out.memory_[kv.first] = kv.second;
    for (const auto &kv : other.memory_) {
      auto it = out.memory_.find(kv.first);
      if (it != out.memory_.end())
        it->second = it->second.join(kv.second);
      else
        out.memory_[kv.first] = kv.second;
    }
    return out;
  }

  OctagonState widen(const OctagonState &other) const {
    if (isBottom_)
      return other;
    if (other.isBottom_)
      return *this;
    if (varToIndex_ != other.varToIndex_)
      return join(other);
    OctagonState out;
    out.varToIndex_ = varToIndex_;
    out.matrix_ = matrix_.widenSimple(other.matrix_);
    for (const auto &kv : memory_)
      out.memory_[kv.first] = kv.second;
    for (const auto &kv : other.memory_) {
      auto it = out.memory_.find(kv.first);
      if (it != out.memory_.end())
        it->second = it->second.widen(kv.second);
      else
        out.memory_[kv.first] = kv.second;
    }
    return out;
  }

  bool operator==(const OctagonState &o) const {
    return isBottom_ == o.isBottom_ && varToIndex_ == o.varToIndex_ &&
           matrix_.dim() == o.matrix_.dim() && memory_ == o.memory_;
  }

  /// Print the state summary to an output stream.
  void print(llvm::raw_ostream &out) const;

  /// True if strong closure has a negative self-loop.
  /// The strong closure is computed lazily and cached to avoid O(n³)
  /// recomputation on every isBottom() call in the hot path of the DAG
  /// interpreter.
  bool hasNegativeSelfLoop() const {
    if (!closureCache_) {
      closureCache_ = matrix_.strongClosure();
    }
    return closureCache_->hasNegativeSelfLoop();
  }

  /// Invalidate the cached strong closure (call after mutating matrix_).
  void invalidateClosureCache() { closureCache_ = llvm::None; }

private:
  static OctagonMatrix expandAndRearrange(const OctagonMatrix &m,
                                          const std::vector<std::size_t> &copy,
                                          std::size_t n) {
    OctagonMatrix out(n);
    const std::size_t d = 2 * n;
    for (std::size_t i = 0; i < d; ++i)
      for (std::size_t j = 0; j < d; ++j) {
        std::size_t si = copy[i], sj = copy[j];
        if (si < m.dim() && sj < m.dim()) {
          auto c = m.get(si, sj);
          if (c)
            out.set(i, j, *c);
        }
      }
    return out;
  }

  bool isBottom_ = false;
  std::unordered_map<const llvm::Value *, std::size_t> varToIndex_;
  OctagonMatrix matrix_;
  std::unordered_map<const llvm::Value *, Interval> memory_;
  /// Lazily computed strong closure cache. Mutable so hasNegativeSelfLoop() is
  /// const.
  mutable llvm::Optional<OctagonMatrix> closureCache_;
};

/// Octagon domain implementing AbstractDomain<Transition, OctagonState>.
/// When BlockTransferPolicy marks a block as block-wise, post(Edge) uses
/// applyBlockWiseHavoc (add all defined values as unconstrained).
class OctagonDomain final : public AbstractDomain<Transition, OctagonState> {
public:
  OctagonDomain() = default;
  explicit OctagonDomain(const BlockTransferPolicy *policy)
      : blockTransferPolicy_(policy) {}
  OctagonDomain(const BlockTransferPolicy *policy,
                lotus::AliasAnalysisWrapper *aliasAnalysis)
      : blockTransferPolicy_(policy), aliasAnalysis_(aliasAnalysis) {}

  void setBlockTransferPolicy(const BlockTransferPolicy *policy) {
    blockTransferPolicy_ = policy;
  }
  const BlockTransferPolicy *getBlockTransferPolicy() const {
    return blockTransferPolicy_;
  }
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
    aliasAnalysis_ = aa;
  }
  lotus::AliasAnalysisWrapper *getAliasAnalysis() const {
    return aliasAnalysis_;
  }

  OctagonState top() const override { return OctagonState(false); }
  OctagonState bottom() const override { return OctagonState(true); }
  bool isBottom(const OctagonState &s) const override {
    return s.isBottom() || s.hasNegativeSelfLoop();
  }
  bool leq(const OctagonState &a, const OctagonState &b) const override {
    if (a.isBottom())
      return true;
    if (b.isBottom())
      return false;
    // a ⊑ b iff every constraint in a is implied by b, i.e. for all (i,j):
    // b.matrix[i][j] >= a.matrix[i][j] (looser or equal bound).
    // We compare after strong closure so both are in canonical form.
    // Variables must be aligned: if a has a variable not in b, we cannot
    // determine containment — conservatively return false.
    for (const auto &kv : a.varToIndex()) {
      if (!b.varToIndex().count(kv.first))
        return false;
    }
    const OctagonMatrix aClosed = a.matrix().strongClosure();
    const OctagonMatrix bClosed = b.matrix().strongClosure();
    // For each pair of indices in a, check that b's bound is >= a's bound.
    for (const auto &kvA : a.varToIndex()) {
      auto itB = b.varToIndex().find(kvA.first);
      if (itB == b.varToIndex().end())
        return false;
      std::size_t iA = kvA.second, iB = itB->second;
      for (const auto &kvA2 : a.varToIndex()) {
        auto itB2 = b.varToIndex().find(kvA2.first);
        if (itB2 == b.varToIndex().end())
          return false;
        std::size_t jA = kvA2.second, jB = itB2->second;
        for (int si = 0; si < 2; ++si) {
          for (int sj = 0; sj < 2; ++sj) {
            auto ca = aClosed.get(2 * iA + si, 2 * jA + sj);
            auto cb = bClosed.get(2 * iB + si, 2 * jB + sj);
            // a has a finite constraint that b does not have (b is +inf): a ⊄
            // b.
            if (ca && !cb)
              return false;
            // Both finite: a's bound must be <= b's bound (b is looser or
            // equal).
            if (ca && cb && *ca > *cb)
              return false;
          }
        }
      }
    }
    return true;
  }
  OctagonState join(const OctagonState &a,
                    const OctagonState &b) const override {
    return a.join(b);
  }
  OctagonState widen(const OctagonState &prev,
                     const OctagonState &next) const override {
    return prev.widen(next);
  }
  /// Apply block transfer (copy/constant/affine; non-linear ops havoc).
  /// Implemented in OctagonDomain.cpp.
  OctagonState applyBlockTransfer(llvm::BasicBlock *bb,
                                  const OctagonState &in) const;
  OctagonState applyBlockTransfer(llvm::BasicBlock *bb, const OctagonState &in,
                                  const llvm::Instruction *segmentStart,
                                  const llvm::Instruction *stopBefore) const;
  /// Block-wise fast path: add all values defined in \p bb as unconstrained
  /// (top).
  OctagonState applyBlockWiseHavoc(llvm::BasicBlock *bb,
                                   const OctagonState &in) const;
  OctagonState applyBlockWiseHavoc(llvm::BasicBlock *bb, const OctagonState &in,
                                   const llvm::Instruction *segmentStart,
                                   const llvm::Instruction *stopBefore) const;
  OctagonState post(const Transition &t, const OctagonState &in) const override;
  OctagonState postCall(const Transition &t,
                        const OctagonState &callerState) const override;
  OctagonState postReturn(const Transition &t, const OctagonState &callerState,
                          const OctagonState &calleeSummary) const override;

private:
  const BlockTransferPolicy *blockTransferPolicy_ = nullptr;
  lotus::AliasAnalysisWrapper *aliasAnalysis_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_OCTAGONDOMAIN_H

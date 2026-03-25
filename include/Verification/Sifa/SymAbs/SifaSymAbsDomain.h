//===-- Verification/Sifa/SymAbs/SifaSymAbsDomain.h -----------------------===//
//
// SymAbsAI-backed Sifa domain implemented using whole-block CFG edges, with an
// SMT fallback for segmented intra-block transfers that do not fit
// bestTransformer's fragment shape.
//
// The public SifaSymAbs helpers remain intraprocedural, but this domain lazily
// materializes per-function SymAbsAI state so EnterCall / ReturnSummary
// transitions can preserve direct-call interprocedural semantics when used via
// IcfgInterpreter.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>

namespace symabs_ai {
class FunctionContext;
class FragmentDecomposition;
class ModuleContext;
} // namespace symabs_ai

namespace lotus {
namespace sifa {

using SymAbsState = std::shared_ptr<symabs_ai::AbstractValue>;

class SifaSymAbsDomain final : public AbstractDomain<Transition, SymAbsState> {
public:
  using Label = Transition;
  using State = SymAbsState;

  /// post() logs progress via SifaLogger when log level is Debug or higher.
  SifaSymAbsDomain(const symabs_ai::FunctionContext &fctx,
                   const symabs_ai::DomainConstructor &domainCtor,
                   const symabs_ai::Analyzer &analyzer)
      : fctx_(fctx), domainCtor_(domainCtor), analyzer_(analyzer) {}
  ~SifaSymAbsDomain() override;

  State top() const override;
  State bottom() const override { return nullptr; }
  bool isBottom(const State &s) const override { return !s || s->isBottom(); }

  bool leq(const State &a, const State &b) const override {
    if (isBottom(a))
      return true;
    if (isBottom(b))
      return isBottom(a);
    return (*a) <= (*b);
  }

  State join(const State &a, const State &b) const override {
    if (isBottom(a))
      return b;
    if (isBottom(b))
      return a;
    std::unique_ptr<symabs_ai::AbstractValue> out(a->clone());
    out->joinWith(*b);
    return State(out.release());
  }

  State widen(const State &previous, const State &next) const override {
    if (isBottom(previous))
      return next;
    if (isBottom(next))
      return previous;
    std::unique_ptr<symabs_ai::AbstractValue> out(previous->clone());
    out->joinWith(*next);
    out->widen();
    return State(out.release());
  }

  State post(const Label &t, const State &in) const override;
  State postCall(const Label &t, const State &callerState) const override;

  /// Create a location-appropriate bottom value (SymAbsAI makeBottom).
  State makeBottomAt(llvm::BasicBlock *bb, bool after) const;

  /// Create a location-appropriate top value.
  State makeTopAt(llvm::BasicBlock *bb, bool after) const;

private:
  struct Bundle {
    std::unique_ptr<symabs_ai::FunctionContext> fctx;
    std::unique_ptr<symabs_ai::FragmentDecomposition> fragDecomp;
    symabs_ai::DomainConstructor domainCtor;
    std::unique_ptr<symabs_ai::Analyzer> analyzer;
  };

  State fallbackPost(const Label &t, const State &in) const;
  State fallbackReturnSummary(const Label &t, const State &in) const;
  State projectEnterCall(const Label &t, const State &callerState) const;
  bool supportsBestTransformer(const Label &t) const;
  const Bundle &bundleFor(const llvm::Function *fn) const;
  const Bundle &bundleFor(const Label &t) const;
  const Bundle &bundleFor(llvm::BasicBlock *bb) const;
  State makeBottomAt(const Bundle &bundle, llvm::BasicBlock *bb,
                     bool after) const;
  State makeTopAt(const Bundle &bundle, llvm::BasicBlock *bb, bool after) const;
  const symabs_ai::FunctionContext &rootContext() const { return fctx_; }

  const symabs_ai::FunctionContext &fctx_;
  const symabs_ai::DomainConstructor &domainCtor_;
  const symabs_ai::Analyzer &analyzer_;
  mutable std::uint64_t postCount_ = 0;
  mutable std::unordered_map<const llvm::Function *, std::unique_ptr<Bundle>>
      bundles_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SYMABS_SIFASYMABSDOMAIN_H

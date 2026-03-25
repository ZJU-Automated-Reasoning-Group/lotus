//===-- Verification/Sifa/Sifa.h ------------------------------------------===//
//
// Sifa: Symbolic Interpretation with Fluid Abstractions (ported from Ultimate
// Library-Sifa). Implements the symbolic interpretation engine from TACAS 2020
// "Ultimate Taipan with Symbolic Interpretation and Fluid Abstractions"
// (Dietsch et al.), combining algebraic program analysis and logical
// interpretation.
//
// Domain selection (Ultimate-aligned): isReachable/isReachableInterprocedural
// use ReachabilityDomain by default. Value domains (Interval, Octagon, etc.)
// in include/Verification/Sifa/Domain/ are available via analyzeTo<StateT>(...)
// or the convenience APIs (e.g. analyzeToWithIntervalDomain).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SIFA_H
#define LOTUS_VERIFICATION_SIFA_SIFA_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"

#include "Verification/Sifa/BlockTransferPolicy.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Domain/EqDomain.h"
#include "Verification/Sifa/Domain/ExplicitValueDomain.h"
#include "Verification/Sifa/Domain/IntervalDomain.h"
#include "Verification/Sifa/Domain/OctagonDomain.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Fluid/IFluid.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Interpreter/IcfgInterpreter.h"
#include "Verification/Sifa/Interpreter/RegexInterpreter.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"

#include <cstddef>

// Needed for getEntryBlock() in the header-only analyzeTo().
#include "llvm/IR/Function.h"

namespace llvm {
class BasicBlock;
class Function;
class Module;
} // namespace llvm

namespace lotus {
class AliasAnalysisWrapper;
namespace sifa {

/// Domain kind for Sifa (Ultimate-aligned:
/// SifaPreferences.LABEL_ABSTRACT_DOMAIN). isReachable /
/// isReachableInterprocedural use Reachability only. For value domains, call
/// the corresponding analyzeToWith*Domain API or switch on domainKind to choose
/// (e.g. in a driver that dispatches to the right API).
enum class SifaDomainKind {
  Reachability,
  Interval,
  Octagon,
  Eq,
  ExplicitValue,
};

struct SifaOptions {
  InterpreterOptions interpreter;
  /// Domain to use when supported by the API. isReachable /
  /// isReachableInterprocedural use Reachability only. For value domains, use
  /// analyzeToWith*Domain or switch on domainKind in your code to call the
  /// right API.
  SifaDomainKind domainKind = SifaDomainKind::Reachability;
  /// Optional per-block transfer strategy. When set, blocks in the policy's
  /// "block-wise" set use a fast havoc transfer; others use
  /// instruction-by-instruction. Enables precision-performance trade-offs (e.g.
  /// block-wise for hot/large blocks).
  llvm::Optional<BlockTransferPolicy> blockTransferPolicy;
  /// Optional alias analysis (lib/Alias). When set with a value domain
  /// (Interval, Octagon, etc.), enables region-based memory: Load/Store use AA
  /// to resolve pointers to regions (allocas, globals) for sound transfer.
  /// IKOS/CLAM style.
  lotus::AliasAnalysisWrapper *aliasAnalysis = nullptr;

  /// Log verbosity for Sifa analysis. Use SifaLogger::setLevel/setOutputStream.
  SifaLogLevel logLevel = SifaLogLevel::None;
};

/// Analyze all paths from entry to \p target and return the resulting abstract
/// state.
template <typename StateT>
StateT analyzeTo(const llvm::Function &F, const llvm::BasicBlock &target,
                 const StateT &initial,
                 const AbstractDomain<Transition, StateT> &domain,
                 const IFluid<StateT> &fluid, SifaOptions options = {}) {
  SifaLogger::setLevel(options.logLevel);

  SifaStats stats;
  DagInterpreter<Transition, StateT> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, StateT> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  const ProcedureResources res(stats, F,
                               {const_cast<llvm::BasicBlock *>(&target)});
  return ipr.interpretForSingleMarker(res.getRegexDag(),
                                      res.getDagOverlayPathToLois(), initial);
}

template <typename StateT>
StateT analyzeTo(const llvm::Function &F, const llvm::BasicBlock &target,
                 const StateT &initial,
                 const AbstractDomain<Transition, StateT> &domain,
                 SifaOptions options = {}) {
  NeverFluid<StateT> fluid;
  return analyzeTo(F, target, initial, domain, fluid, options);
}

/// Convenience API: procedure-level reachability via Sifa's path-expression
/// interpreter.
bool isReachable(const llvm::Function &F, const llvm::BasicBlock &target,
                 SifaOptions options = {});

/// Analyze an interprocedural query and return the abstract state at
/// `targetBlock` in `targetFunc`.
template <typename StateT>
StateT analyzeInterproceduralTo(
    const llvm::Module &M, llvm::ArrayRef<const llvm::Function *> entries,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const StateT &initial, const AbstractDomain<Transition, StateT> &domain,
    const IFluid<StateT> &fluid, const SifaOptions &options = {}) {
  SifaLogger::setLevel(options.logLevel);

  SifaStats stats;
  std::vector<CallGraph::LOI> lois = {{&targetFunc, &targetBlock}};
  IcfgInterpreter<StateT> icfg(M, entries, lois, stats, domain, fluid, initial);
  MapBasedStorage<const llvm::BasicBlock *, StateT> storage;
  icfg.interpret(storage);

  auto *bb = const_cast<llvm::BasicBlock *>(&targetBlock);
  auto it = storage.getMap().find(bb);
  return it != storage.getMap().end() ? it->second : domain.bottom();
}

template <typename StateT>
StateT analyzeInterproceduralTo(
    const llvm::Module &M, llvm::ArrayRef<const llvm::Function *> entries,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const StateT &initial, const AbstractDomain<Transition, StateT> &domain,
    const SifaOptions &options = {}) {
  NeverFluid<StateT> fluid;
  return analyzeInterproceduralTo(M, entries, targetFunc, targetBlock, initial,
                                  domain, fluid, options);
}

template <typename StateT>
StateT analyzeInterproceduralTo(
    const llvm::Module &M, const llvm::Function *entry,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const StateT &initial, const AbstractDomain<Transition, StateT> &domain,
    const IFluid<StateT> &fluid, const SifaOptions &options = {}) {
  llvm::ArrayRef<const llvm::Function *> entries =
      entry ? llvm::ArrayRef<const llvm::Function *>{entry}
            : llvm::ArrayRef<const llvm::Function *>{};
  return analyzeInterproceduralTo(M, entries, targetFunc, targetBlock, initial,
                                  domain, fluid, options);
}

template <typename StateT>
StateT analyzeInterproceduralTo(
    const llvm::Module &M, const llvm::Function *entry,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const StateT &initial, const AbstractDomain<Transition, StateT> &domain,
    const SifaOptions &options = {}) {
  NeverFluid<StateT> fluid;
  return analyzeInterproceduralTo(M, entry, targetFunc, targetBlock, initial,
                                  domain, fluid, options);
}

/// Interprocedural reachability: \p targetBlock in \p targetFunc reachable from
/// \p entry.
bool isReachableInterprocedural(const llvm::Module &M,
                                llvm::ArrayRef<const llvm::Function *> entries,
                                const llvm::Function &targetFunc,
                                const llvm::BasicBlock &targetBlock,
                                const IFluid<bool> &fluid,
                                const SifaOptions &options = {});
bool isReachableInterprocedural(const llvm::Module &M,
                                llvm::ArrayRef<const llvm::Function *> entries,
                                const llvm::Function &targetFunc,
                                const llvm::BasicBlock &targetBlock,
                                const SifaOptions &options = {});
/// Interprocedural reachability: \p targetBlock in \p targetFunc reachable from
/// \p entry.
bool isReachableInterprocedural(const llvm::Module &M,
                                const llvm::Function *entry,
                                const llvm::Function &targetFunc,
                                const llvm::BasicBlock &targetBlock,
                                const IFluid<bool> &fluid,
                                const SifaOptions &options = {});
bool isReachableInterprocedural(const llvm::Module &M,
                                const llvm::Function *entry,
                                const llvm::Function &targetFunc,
                                const llvm::BasicBlock &targetBlock,
                                const SifaOptions &options = {});

/// Convenience APIs: analyze paths from entry to \p target using the chosen
/// domain. Returns the abstract state at \p target. Use the API that matches
/// the domain you want (or switch on SifaOptions::domainKind to choose at run
/// time).
IntervalState analyzeToWithIntervalDomain(const llvm::Function &F,
                                          const llvm::BasicBlock &target,
                                          const IntervalState &initial,
                                          const IFluid<IntervalState> &fluid,
                                          SifaOptions options = {});
IntervalState analyzeToWithIntervalDomain(const llvm::Function &F,
                                          const llvm::BasicBlock &target,
                                          const IntervalState &initial,
                                          SifaOptions options = {});
OctagonState analyzeToWithOctagonDomain(const llvm::Function &F,
                                        const llvm::BasicBlock &target,
                                        const OctagonState &initial,
                                        const IFluid<OctagonState> &fluid,
                                        SifaOptions options = {});
OctagonState analyzeToWithOctagonDomain(const llvm::Function &F,
                                        const llvm::BasicBlock &target,
                                        const OctagonState &initial,
                                        SifaOptions options = {});
EqState analyzeToWithEqDomain(const llvm::Function &F,
                              const llvm::BasicBlock &target,
                              const EqState &initial,
                              const IFluid<EqState> &fluid,
                              SifaOptions options = {});
EqState analyzeToWithEqDomain(const llvm::Function &F,
                              const llvm::BasicBlock &target,
                              const EqState &initial, SifaOptions options = {});
ExplicitValueState analyzeToWithExplicitValueDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const ExplicitValueState &initial, const IFluid<ExplicitValueState> &fluid,
    SifaOptions options = {});
ExplicitValueState analyzeToWithExplicitValueDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const ExplicitValueState &initial, SifaOptions options = {});

/// Analyze to procedure exit (join of all return paths).
/// Instruction-by-instruction transfer, no SMT. Use these for fast default
/// analysis.
IntervalState analyzeToReturnWithIntervalDomain(
    const llvm::Function &F, const IntervalState &initial,
    const IFluid<IntervalState> &fluid, SifaOptions options = {});
IntervalState analyzeToReturnWithIntervalDomain(const llvm::Function &F,
                                                const IntervalState &initial,
                                                SifaOptions options = {});
OctagonState analyzeToReturnWithOctagonDomain(const llvm::Function &F,
                                              const OctagonState &initial,
                                              const IFluid<OctagonState> &fluid,
                                              SifaOptions options = {});
OctagonState analyzeToReturnWithOctagonDomain(const llvm::Function &F,
                                              const OctagonState &initial,
                                              SifaOptions options = {});

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SIFA_H

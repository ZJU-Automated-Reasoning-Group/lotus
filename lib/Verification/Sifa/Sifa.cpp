// Sifa: Symbolic Interpretation with Fluid Abstractions.
// Algorithm from TACAS 2020 "Ultimate Taipan with Symbolic Interpretation and
// Fluid Abstractions" (Dietsch et al.) — ICFG/DAG interpreters, RegexDAG,
// post operator, call/loop summarization, fluid abstraction policies.
#include "Verification/Sifa/Sifa.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "Verification/Sifa/Domain/EqDomain.h"
#include "Verification/Sifa/Domain/ExplicitValueDomain.h"
#include "Verification/Sifa/Domain/IntervalDomain.h"
#include "Verification/Sifa/Domain/OctagonDomain.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Interpreter/IcfgInterpreter.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"

using namespace lotus::sifa;

bool lotus::sifa::isReachable(const llvm::Function &F,
                              const llvm::BasicBlock &target,
                              SifaOptions options) {
  ReachabilityDomain<Transition> domain;
  return analyzeTo<bool>(F, target, /*initial=*/true, domain, options);
}

bool lotus::sifa::isReachableInterprocedural(
    const llvm::Module &M, llvm::ArrayRef<const llvm::Function *> entries,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const IFluid<bool> &fluid, const SifaOptions &options) {
  SifaLogger::setLevel(options.logLevel);

  SifaStats stats;
  ReachabilityDomain<Transition> domain;

  std::vector<std::pair<const llvm::Function *, const llvm::BasicBlock *>>
      lois = {{&targetFunc, &targetBlock}};
  IcfgInterpreter<bool> icfg(M, entries, lois, stats, domain, fluid,
                             /*initialState=*/true);
  MapBasedStorage<const llvm::BasicBlock *, bool> storage;
  icfg.interpret(storage);
  auto *bb = const_cast<llvm::BasicBlock *>(&targetBlock);
  auto it = storage.getMap().find(bb);
  return it != storage.getMap().end() && it->second;
}

bool lotus::sifa::isReachableInterprocedural(
    const llvm::Module &M, llvm::ArrayRef<const llvm::Function *> entries,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const SifaOptions &options) {
  NeverFluid<bool> fluid;
  return isReachableInterprocedural(M, entries, targetFunc, targetBlock, fluid,
                                    options);
}

bool lotus::sifa::isReachableInterprocedural(
    const llvm::Module &M, const llvm::Function *entry,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const IFluid<bool> &fluid, const SifaOptions &options) {
  llvm::ArrayRef<const llvm::Function *> entries =
      entry ? llvm::ArrayRef<const llvm::Function *>{entry}
            : llvm::ArrayRef<const llvm::Function *>{};
  return isReachableInterprocedural(M, entries, targetFunc, targetBlock, fluid,
                                    options);
}

bool lotus::sifa::isReachableInterprocedural(
    const llvm::Module &M, const llvm::Function *entry,
    const llvm::Function &targetFunc, const llvm::BasicBlock &targetBlock,
    const SifaOptions &options) {
  NeverFluid<bool> fluid;
  return isReachableInterprocedural(M, entry, targetFunc, targetBlock, fluid,
                                    options);
}

IntervalState lotus::sifa::analyzeToWithIntervalDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const IntervalState &initial, const IFluid<IntervalState> &fluid,
    SifaOptions options) {
  IntervalDomain domain(options.blockTransferPolicy.hasValue()
                            ? &*options.blockTransferPolicy
                            : nullptr,
                        options.aliasAnalysis);
  return analyzeTo<IntervalState>(F, target, initial, domain, fluid, options);
}

IntervalState lotus::sifa::analyzeToWithIntervalDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const IntervalState &initial, SifaOptions options) {
  NeverFluid<IntervalState> fluid;
  return analyzeToWithIntervalDomain(F, target, initial, fluid, options);
}

OctagonState lotus::sifa::analyzeToWithOctagonDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const OctagonState &initial, const IFluid<OctagonState> &fluid,
    SifaOptions options) {
  OctagonDomain domain(options.blockTransferPolicy.hasValue()
                           ? &*options.blockTransferPolicy
                           : nullptr,
                       options.aliasAnalysis);
  return analyzeTo<OctagonState>(F, target, initial, domain, fluid, options);
}

OctagonState lotus::sifa::analyzeToWithOctagonDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const OctagonState &initial, SifaOptions options) {
  NeverFluid<OctagonState> fluid;
  return analyzeToWithOctagonDomain(F, target, initial, fluid, options);
}

EqState lotus::sifa::analyzeToWithEqDomain(const llvm::Function &F,
                                           const llvm::BasicBlock &target,
                                           const EqState &initial,
                                           const IFluid<EqState> &fluid,
                                           SifaOptions options) {
  EqDomain domain(options.blockTransferPolicy.hasValue()
                      ? &*options.blockTransferPolicy
                      : nullptr,
                  options.aliasAnalysis);
  return analyzeTo<EqState>(F, target, initial, domain, fluid, options);
}

EqState lotus::sifa::analyzeToWithEqDomain(const llvm::Function &F,
                                           const llvm::BasicBlock &target,
                                           const EqState &initial,
                                           SifaOptions options) {
  NeverFluid<EqState> fluid;
  return analyzeToWithEqDomain(F, target, initial, fluid, options);
}

ExplicitValueState lotus::sifa::analyzeToWithExplicitValueDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const ExplicitValueState &initial, const IFluid<ExplicitValueState> &fluid,
    SifaOptions options) {
  ExplicitValueDomain domain(options.blockTransferPolicy.hasValue()
                                 ? &*options.blockTransferPolicy
                                 : nullptr,
                             options.aliasAnalysis);
  return analyzeTo<ExplicitValueState>(F, target, initial, domain, fluid,
                                       options);
}

ExplicitValueState lotus::sifa::analyzeToWithExplicitValueDomain(
    const llvm::Function &F, const llvm::BasicBlock &target,
    const ExplicitValueState &initial, SifaOptions options) {
  NeverFluid<ExplicitValueState> fluid;
  return analyzeToWithExplicitValueDomain(F, target, initial, fluid, options);
}

static IntervalState runToReturnWithIntervalDomain(
    const llvm::Function &F, const IntervalState &initial,
    const IFluid<IntervalState> &fluid, SifaOptions options) {
  SifaStats stats;
  IntervalDomain domain(options.blockTransferPolicy.hasValue()
                            ? &*options.blockTransferPolicy
                            : nullptr,
                        options.aliasAnalysis);
  DagInterpreter<Transition, IntervalState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, IntervalState> loopSum(stats, domain,
                                                            fluid, ipr);
  ipr.setLoopSummarizer(loopSum);
  ProcedureResources res(stats, F, std::vector<llvm::BasicBlock *>{});
  return ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToReturn(), initial);
}

IntervalState lotus::sifa::analyzeToReturnWithIntervalDomain(
    const llvm::Function &F, const IntervalState &initial,
    const IFluid<IntervalState> &fluid, SifaOptions options) {
  return runToReturnWithIntervalDomain(F, initial, fluid, options);
}

IntervalState
lotus::sifa::analyzeToReturnWithIntervalDomain(const llvm::Function &F,
                                               const IntervalState &initial,
                                               SifaOptions options) {
  NeverFluid<IntervalState> fluid;
  return runToReturnWithIntervalDomain(F, initial, fluid, options);
}

static OctagonState runToReturnWithOctagonDomain(
    const llvm::Function &F, const OctagonState &initial,
    const IFluid<OctagonState> &fluid, SifaOptions options) {
  SifaStats stats;
  OctagonDomain domain(options.blockTransferPolicy.hasValue()
                           ? &*options.blockTransferPolicy
                           : nullptr,
                       options.aliasAnalysis);
  DagInterpreter<Transition, OctagonState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, OctagonState> loopSum(stats, domain, fluid,
                                                           ipr);
  ipr.setLoopSummarizer(loopSum);
  ProcedureResources res(stats, F, std::vector<llvm::BasicBlock *>{});
  return ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToReturn(), initial);
}

OctagonState lotus::sifa::analyzeToReturnWithOctagonDomain(
    const llvm::Function &F, const OctagonState &initial,
    const IFluid<OctagonState> &fluid, SifaOptions options) {
  return runToReturnWithOctagonDomain(F, initial, fluid, options);
}

OctagonState lotus::sifa::analyzeToReturnWithOctagonDomain(
    const llvm::Function &F, const OctagonState &initial, SifaOptions options) {
  NeverFluid<OctagonState> fluid;
  return runToReturnWithOctagonDomain(F, initial, fluid, options);
}

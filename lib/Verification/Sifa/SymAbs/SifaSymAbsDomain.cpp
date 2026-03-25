#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/InstructionSemantics.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace lotus::sifa;

namespace {

const llvm::Instruction *firstNonPhi(const llvm::BasicBlock *bb) {
  if (!bb)
    return nullptr;
  for (const llvm::Instruction &I : *bb) {
    if (!llvm::isa<llvm::PHINode>(&I))
      return &I;
  }
  return nullptr;
}

llvm::BasicBlock *transitionTarget(const Transition &t) {
  return t.target ? t.target : symabs_ai::Fragment::EXIT;
}

bool isBlockEntryPoint(const Transition &t) {
  return t.sourceOrdinal == 0 && (!t.target || t.targetOrdinal == 0);
}

const llvm::Function *owningFunction(llvm::BasicBlock *bb) {
  if (!bb || bb == symabs_ai::Fragment::EXIT) {
    return nullptr;
  }
  return bb->getParent();
}

z3::expr valueExprAtCall(const symabs_ai::FunctionContext &fctx,
                        const symabs_ai::ValueMapping &vm,
                        llvm::Value *value) {
  if (fctx.isRepresentedValue(value)) {
    return vm.getFullRepresentation(value);
  }
  if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
    return makeConstantInt(&fctx.getZ3(), constant);
  }
  if (llvm::isa<llvm::ConstantPointerNull>(value)) {
    return fctx.getMemoryModel().make_nullptr();
  }
  return vm.getFullRepresentation(value);
}

z3::expr directCallSummaryFormula(const symabs_ai::FunctionContext &fctx,
                                  const symabs_ai::ValueMapping &vmBefore,
                                  const symabs_ai::ValueMapping &vmAfter,
                                  const llvm::CallBase &call) {
  auto &z3 = fctx.getZ3();
  llvm::Function *callee = call.getCalledFunction();
  if (!callee) {
    return z3.bool_val(true);
  }

  const auto &mctx = fctx.getModuleContext();
  z3::expr rawExpr = mctx.formulaFor(callee);
  z3::expr_vector src(z3), dst(z3);

  auto *formalIt = callee->arg_begin();
  auto *formalEnd = callee->arg_end();
  const auto *actualIt = call.arg_begin();
  const auto *actualEnd = call.arg_end();
  for (; formalIt != formalEnd && actualIt != actualEnd;
       ++formalIt, ++actualIt) {
    llvm::Value *actual = *actualIt;
    if (actual->getType()->isMetadataTy()) {
      continue;
    }
    z3::expr actualExpr = vmBefore.getFullRepresentation(actual);
    src.push_back(
        z3.constant(formalIt->getName().str().c_str(), actualExpr.get_sort()));
    dst.push_back(actualExpr);
  }

  if (!call.getType()->isVoidTy()) {
    z3::expr resultExpr =
        vmAfter.getFullRepresentation(const_cast<llvm::CallBase *>(&call));
    src.push_back(z3.constant(mctx.getReturnSymbol(), resultExpr.get_sort()));
    dst.push_back(resultExpr);
  }

  z3::expr result = rawExpr.substitute(src, dst);
  if (callee->onlyReadsMemory() || callee->doesNotAccessMemory() ||
      call.getMetadata("symbolic_abstraction")) {
    result = result && (vmBefore.memory() == vmAfter.memory());
  }
  return result;
}

z3::expr
preserveUnchangedRepresentedValues(const symabs_ai::FunctionContext &fctx,
                                   const symabs_ai::Fragment &frag,
                                   const symabs_ai::ValueMapping &vmBefore,
                                   const symabs_ai::ValueMapping &vmAfter) {
  z3::expr preserved = fctx.getZ3().bool_val(true);
  for (const auto &value : fctx.representedValues()) {
    llvm::Value *llvmValue = value;
    if (!frag.defines(llvmValue)) {
      preserved = preserved && (vmBefore.getFullRepresentation(llvmValue) ==
                                vmAfter.getFullRepresentation(llvmValue));
    }
  }
  return preserved;
}

} // namespace

SifaSymAbsDomain::~SifaSymAbsDomain() = default;

SymAbsState SifaSymAbsDomain::top() const {
  llvm::Function *fn = fctx_.getFunction();
  if (!fn || fn->empty())
    return nullptr;
  llvm::BasicBlock *entry = &*fn->begin();
  return makeTopAt(entry, /*after=*/false);
}

const SifaSymAbsDomain::Bundle &
SifaSymAbsDomain::bundleFor(const llvm::Function *fn) const {
  if (!fn || fn == fctx_.getFunction()) {
    static Bundle rootBundle{nullptr, nullptr, symabs_ai::DomainConstructor(),
                             nullptr};
    return rootBundle;
  }

  auto it = bundles_.find(fn);
  if (it != bundles_.end()) {
    return *it->second;
  }

  auto bundle = std::make_unique<Bundle>();
  auto fctx = fctx_.getModuleContext().createFunctionContext(
      const_cast<llvm::Function *>(fn));
  bundle->domainCtor = symabs_ai::DomainConstructor(fctx->getConfig());
  auto fragDecomp =
      std::make_unique<symabs_ai::FragmentDecomposition>(
          symabs_ai::FragmentDecomposition::For(*fctx));
  bundle->analyzer =
      symabs_ai::Analyzer::New(*fctx, *fragDecomp, bundle->domainCtor);
  bundle->fctx = std::move(fctx);
  bundle->fragDecomp = std::move(fragDecomp);

  auto inserted = bundles_.emplace(fn, std::move(bundle));
  return *inserted.first->second;
}

const SifaSymAbsDomain::Bundle &
SifaSymAbsDomain::bundleFor(const Label &t) const {
  if (const llvm::Function *fn = owningFunction(t.source)) {
    if (fn != fctx_.getFunction()) {
      return bundleFor(fn);
    }
  }
  if (const llvm::Function *fn = owningFunction(t.target)) {
    if (fn != fctx_.getFunction()) {
      return bundleFor(fn);
    }
  }
  if (t.callee && t.callee != fctx_.getFunction()) {
    return bundleFor(t.callee);
  }
  static Bundle rootBundle{nullptr, nullptr, symabs_ai::DomainConstructor(),
                           nullptr};
  return rootBundle;
}

const SifaSymAbsDomain::Bundle &
SifaSymAbsDomain::bundleFor(llvm::BasicBlock *bb) const {
  if (const llvm::Function *fn = owningFunction(bb)) {
    if (fn != fctx_.getFunction()) {
      return bundleFor(fn);
    }
  }
  static Bundle rootBundle{nullptr, nullptr, symabs_ai::DomainConstructor(),
                           nullptr};
  return rootBundle;
}

SymAbsState SifaSymAbsDomain::makeBottomAt(const Bundle &bundle,
                                           llvm::BasicBlock *bb,
                                           bool after) const {
  if (bundle.fctx) {
    auto v = bundle.domainCtor.makeBottom(*bundle.fctx, bb, after);
    return SymAbsState(v.release());
  }
  auto v = domainCtor_.makeBottom(fctx_, bb, after);
  return SymAbsState(v.release());
}

SymAbsState SifaSymAbsDomain::makeTopAt(const Bundle &bundle,
                                        llvm::BasicBlock *bb,
                                        bool after) const {
  SymAbsState v = makeBottomAt(bundle, bb, after);
  if (v) {
    v->havoc();
  }
  return v;
}

SymAbsState SifaSymAbsDomain::makeBottomAt(llvm::BasicBlock *bb,
                                           bool after) const {
  return makeBottomAt(bundleFor(bb), bb, after);
}

SymAbsState SifaSymAbsDomain::makeTopAt(llvm::BasicBlock *bb,
                                        bool after) const {
  return makeTopAt(bundleFor(bb), bb, after);
}

bool SifaSymAbsDomain::supportsBestTransformer(const Label &t) const {
  if (t.kind != TransitionKind::Edge || !t.source)
    return false;
  if (t.stopBefore != nullptr)
    return false;
  if (!isBlockEntryPoint(t))
    return false;
  const llvm::Instruction *blockStart = firstNonPhi(t.source);
  return t.segmentStart == nullptr || t.segmentStart == blockStart;
}

SymAbsState SifaSymAbsDomain::fallbackReturnSummary(const Label &t,
                                                    const State &in) const {
  const Bundle &bundle = bundleFor(owningFunction(t.source));
  const symabs_ai::FunctionContext &fctx = bundle.fctx ? *bundle.fctx : fctx_;
  const symabs_ai::Analyzer &analyzer = bundle.analyzer ? *bundle.analyzer
                                                        : analyzer_;
  const auto *call = llvm::dyn_cast_or_null<llvm::CallBase>(t.call);
  if (!call) {
    return makeTopAt(bundle, transitionTarget(t), /*after=*/false);
  }
  if (const auto *callInst = llvm::dyn_cast<llvm::CallInst>(call)) {
    auto frag =
        symabs_ai::FragmentDecomposition::FragmentForBody(fctx, t.source);
    symabs_ai::InstructionSemantics instSem(fctx, frag);
    auto vmIn = symabs_ai::ValueMapping::before(
        fctx, frag, const_cast<llvm::CallInst *>(callInst));
    auto vmOut = symabs_ai::ValueMapping::after(
        fctx, frag, const_cast<llvm::CallInst *>(callInst));

    z3::expr phi = in->toFormula(vmIn, fctx.getZ3()) &&
                   instSem.visit(*const_cast<llvm::CallInst *>(callInst));

    auto out = makeBottomAt(bundle, t.source, /*after=*/true);
    analyzer.strongestConsequence(out.get(), phi, vmOut);
    return out;
  }

  if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
    std::set<symabs_ai::Fragment::edge> edges;
    edges.insert({t.source, transitionTarget(t)});
    symabs_ai::Fragment frag(fctx, t.source, transitionTarget(t), edges,
                             /*includes_end_body=*/false);
    symabs_ai::InstructionSemantics instSem(fctx, frag);

    auto vmIn = symabs_ai::ValueMapping::before(
        fctx, frag, const_cast<llvm::InvokeInst *>(invoke));
    auto vmOut = symabs_ai::ValueMapping::atEnd(fctx, frag);

    z3::expr phi = in->toFormula(vmIn, fctx.getZ3());
    phi = phi && directCallSummaryFormula(fctx, vmIn, vmOut, *invoke);
    phi = phi && preserveUnchangedRepresentedValues(fctx, frag, vmIn, vmOut);

    for (llvm::BasicBlock *succ : llvm::successors(t.source)) {
      z3::expr edgeVar = fctx.getEdgeVariable(t.source, succ);
      phi = phi && (succ == t.target ? edgeVar : !edgeVar);
    }
    for (const auto &edge : frag.edges()) {
      for (llvm::Instruction &I : frag.edgePhis(edge)) {
        phi = phi && instSem.visit(I);
      }
    }

    auto out = makeBottomAt(bundle, t.target, /*after=*/false);
    analyzer.strongestConsequence(out.get(), phi, vmOut);
    return out;
  }

  return makeTopAt(bundle, transitionTarget(t), /*after=*/false);
}

SymAbsState SifaSymAbsDomain::projectEnterCall(const Label &t,
                                               const State &callerState) const {
  if (!t.call || !t.callee || !t.source || !t.target) {
    return callerState;
  }

  const Bundle &callerBundle = bundleFor(owningFunction(t.source));
  const Bundle &calleeBundle = bundleFor(t.callee);
  const symabs_ai::FunctionContext &callerFctx =
      callerBundle.fctx ? *callerBundle.fctx : fctx_;
  const symabs_ai::FunctionContext &calleeFctx =
      calleeBundle.fctx ? *calleeBundle.fctx : fctx_;
  const symabs_ai::Analyzer &calleeAnalyzer =
      calleeBundle.analyzer ? *calleeBundle.analyzer : analyzer_;

  auto callerFrag =
      symabs_ai::FragmentDecomposition::FragmentForBody(callerFctx, t.source);
  auto callerVm = symabs_ai::ValueMapping::before(
      callerFctx, callerFrag, const_cast<llvm::Instruction *>(
                                 llvm::cast<llvm::Instruction>(t.call)));
  auto calleeEntryFrag =
      symabs_ai::FragmentDecomposition::FragmentForBody(calleeFctx, t.target);
  auto calleeVm = symabs_ai::ValueMapping::atBeginning(calleeFctx,
                                                       calleeEntryFrag);

  z3::expr phi = callerState->toFormula(callerVm, callerFctx.getZ3());
  phi = phi && (callerVm.memory() == calleeVm.memory());

  unsigned actualIndex = 0;
  for (const auto &formal : t.callee->args()) {
    if (!calleeFctx.isRepresentedValue(const_cast<llvm::Argument *>(&formal))) {
      ++actualIndex;
      continue;
    }
    while (actualIndex < t.call->arg_size() &&
           t.call->getArgOperand(actualIndex)->getType()->isMetadataTy()) {
      ++actualIndex;
    }
    if (actualIndex >= t.call->arg_size()) {
      break;
    }
    llvm::Value *actual = t.call->getArgOperand(actualIndex);
    phi = phi && (calleeVm.getFullRepresentation(
                      const_cast<llvm::Argument *>(&formal)) ==
                  valueExprAtCall(callerFctx, callerVm, actual));
    ++actualIndex;
  }

  for (const auto &represented : calleeFctx.representedValues()) {
    llvm::Value *value = represented;
    if (llvm::isa<llvm::Argument>(value)) {
      continue;
    }
    if (callerFctx.isRepresentedValue(value)) {
      phi = phi && (calleeVm.getFullRepresentation(value) ==
                    callerVm.getFullRepresentation(value));
    }
  }

  auto out = makeBottomAt(calleeBundle, t.target, /*after=*/false);
  calleeAnalyzer.strongestConsequence(out.get(), phi, calleeVm);
  return out;
}

SymAbsState SifaSymAbsDomain::fallbackPost(const Label &t,
                                           const State &in) const {
  if (!t.source) {
    throw std::logic_error("SifaSymAbs fallback requires a source block");
  }

  const Bundle &bundle = bundleFor(owningFunction(t.source));
  const symabs_ai::FunctionContext &fctx = bundle.fctx ? *bundle.fctx : fctx_;
  const symabs_ai::Analyzer &analyzer = bundle.analyzer ? *bundle.analyzer
                                                        : analyzer_;

  const bool crossesEdge = transitionTarget(t) != t.source;
  std::vector<symabs_ai::Fragment::edge> edges;
  if (crossesEdge) {
    edges.push_back({t.source, transitionTarget(t)});
  }

  symabs_ai::Fragment frag(fctx, t.source, transitionTarget(t), edges,
                           /*includes_end_body=*/false);
  symabs_ai::InstructionSemantics instSem(fctx, frag);

  const llvm::Instruction *segmentStart =
      t.segmentStart ? t.segmentStart : firstNonPhi(t.source);
  if (!segmentStart) {
    return makeTopAt(bundle, transitionTarget(t), /*after=*/false);
  }
  if (t.stopBefore == segmentStart) {
    return in;
  }

  auto vmIn = symabs_ai::ValueMapping::before(
      fctx, frag, const_cast<llvm::Instruction *>(segmentStart));
  z3::expr phi = in->toFormula(vmIn, fctx.getZ3());

  for (auto it = const_cast<llvm::Instruction *>(segmentStart)->getIterator(),
            end = t.source->end();
       it != end; ++it) {
    llvm::Instruction &I = *it;
    if (&I == t.stopBefore) {
      break;
    }
    phi = phi && instSem.visit(I);
  }

  if (crossesEdge) {
    const llvm::Instruction *terminator = t.source->getTerminator();
    if (terminator && terminator != t.stopBefore) {
      z3::expr chosen = fctx.getEdgeVariable(t.source, transitionTarget(t));
      for (llvm::BasicBlock *succ : llvm::successors(t.source)) {
        z3::expr edgeVar = fctx.getEdgeVariable(t.source, succ);
        phi = phi && (succ == t.target ? edgeVar : !edgeVar);
      }
      if (llvm::isa<llvm::ReturnInst>(terminator) && !t.target) {
        phi = phi && chosen;
      }

      auto vmBeforeTerm = symabs_ai::ValueMapping::before(
          fctx, frag, const_cast<llvm::Instruction *>(terminator));
      auto vmOut = symabs_ai::ValueMapping::atEnd(fctx, frag);
      phi = phi &&
            fctx.getMemoryModel().copy(vmBeforeTerm.memory(), vmOut.memory());

      for (const auto &edge : frag.edges()) {
        for (llvm::Instruction &I : frag.edgePhis(edge)) {
          phi = phi && instSem.visit(I);
        }
      }

      auto out = makeBottomAt(bundle, t.target, /*after=*/false);
      analyzer.strongestConsequence(out.get(), phi, vmOut);
      return out;
    }
  }

  auto out = makeBottomAt(bundle, t.source, /*after=*/true);
  auto vmOut = t.stopBefore ? symabs_ai::ValueMapping::before(
                                  fctx, frag,
                                  const_cast<llvm::Instruction *>(t.stopBefore))
                            : symabs_ai::ValueMapping::atEnd(fctx, frag);
  analyzer.strongestConsequence(out.get(), phi, vmOut);
  return out;
}

SymAbsState SifaSymAbsDomain::post(const Label &t, const State &in) const {
  if (isBottom(in)) {
    return bottom();
  }
  if (t.kind == TransitionKind::Marker) {
    // Markers are handled in DagInterpreter; treat as identity here.
    return in;
  }
  if (t.kind != TransitionKind::Edge) {
    return fallbackPost(t, in);
  }
  if (!supportsBestTransformer(t)) {
    return fallbackPost(t, in);
  }

  const Bundle &bundle = bundleFor(owningFunction(t.source));
  const symabs_ai::FunctionContext &fctx = bundle.fctx ? *bundle.fctx : fctx_;
  const symabs_ai::Analyzer &analyzer = bundle.analyzer ? *bundle.analyzer
                                                        : analyzer_;
  llvm::BasicBlock *src = t.source;
  llvm::BasicBlock *dst = t.target;

  std::set<symabs_ai::Fragment::edge> edges;
  edges.insert({src, dst});
  const symabs_ai::Fragment frag(fctx, src, dst, edges,
                                 /*includes_end_body=*/false);

  // Result is a bottom at the end location (state at dst after phi nodes).
  auto out = (bundle.fctx ? bundle.domainCtor.makeBottom(fctx, dst, false)
                          : domainCtor_.makeBottom(fctx_, dst, false));
  if (SifaLogger::isEnabled(SifaLogLevel::Debug)) {
    ++postCount_;
    if (postCount_ <= 10 || postCount_ % 25 == 0 || postCount_ == 11) {
      auto srcName =
          src ? (src->getName().empty() ? "(entry)" : src->getName().str())
              : "?";
      auto dstName =
          dst ? (dst->getName().empty() ? "(exit)" : dst->getName().str())
              : "EXIT";
      SifaLogger::debug("bestTransformer #" + std::to_string(postCount_) +
                        ": " + srcName + " -> " + dstName);
    }
  }
  analyzer.bestTransformer(in.get(), frag, out.get());
  return SymAbsState(out.release());
}

SymAbsState SifaSymAbsDomain::postCall(const Label &t,
                                       const State &callerState) const {
  if (isBottom(callerState))
    return bottom();
  if (t.kind == TransitionKind::ReturnSummary && t.call) {
    return fallbackReturnSummary(t, callerState);
  }
  if (t.kind == TransitionKind::EnterCall && t.target) {
    return projectEnterCall(t, callerState);
  }
  return callerState;
}

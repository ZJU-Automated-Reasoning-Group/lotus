#include "Verification/Sifa/SifaSymAbs.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Utils/Config.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lotus::sifa;

namespace {
bool profileEnabled() { return std::getenv("LOTUS_SIFA_PROFILE") != nullptr; }

void logProfileTiming(const char *stage,
                      std::chrono::steady_clock::duration elapsed) {
  if (!profileEnabled()) {
    return;
  }
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  llvm::errs() << "[sifa-profile] " << stage << ": " << ms << " ms\n";
}
} // namespace

static bool isSupportedSymAbsValueType(llvm::Type *ty,
                                       const SifaSymAbsOptions &opt) {
  if (!ty)
    return false;
  if (ty->isVoidTy())
    return true;
  // Non-value/control-only types that can appear as operands (e.g., branch
  // targets).
  if (ty->isLabelTy() || ty->isMetadataTy() || ty->isTokenTy())
    return true;
  if (ty->isPointerTy())
    return true;
  if (ty->isIntegerTy())
    return ty->getIntegerBitWidth() <= 64;
  if (opt.allowDouble && ty->isDoubleTy())
    return true;
  return false;
}

static bool isUnsupportedSymAbsInstruction(const llvm::Instruction &I) {
  // These are either not encoded at all (placeholder `true`) or rely on LLVM
  // exception handling machinery we currently don't model.
  return llvm::isa<llvm::IndirectBrInst>(I) || llvm::isa<llvm::InvokeInst>(I) ||
         llvm::isa<llvm::ResumeInst>(I) || llvm::isa<llvm::LandingPadInst>(I) ||
         llvm::isa<llvm::CleanupReturnInst>(I) ||
         llvm::isa<llvm::CatchReturnInst>(I) ||
         llvm::isa<llvm::CatchSwitchInst>(I) ||
         llvm::isa<llvm::FuncletPadInst>(I) ||
         llvm::isa<llvm::CleanupPadInst>(I) ||
         llvm::isa<llvm::CatchPadInst>(I) || llvm::isa<llvm::FenceInst>(I) ||
         llvm::isa<llvm::AtomicCmpXchgInst>(I) ||
         llvm::isa<llvm::AtomicRMWInst>(I) || llvm::isa<llvm::VAArgInst>(I) ||
         llvm::isa<llvm::ExtractElementInst>(I) ||
         llvm::isa<llvm::InsertElementInst>(I) ||
         llvm::isa<llvm::ShuffleVectorInst>(I) ||
         llvm::isa<llvm::ExtractValueInst>(I) ||
         llvm::isa<llvm::InsertValueInst>(I) ||
         llvm::isa<llvm::AddrSpaceCastInst>(I);
}

static void validateLlvmSubsetOrThrow(const llvm::Module &M,
                                      const llvm::Function &F,
                                      const SifaSymAbsOptions &opt) {
  if (!opt.validateLlvmSubset)
    return;

  std::vector<std::string> issues;
  issues.reserve(16);

  auto addIssue = [&](llvm::Twine msg) {
    if (issues.size() < 20)
      issues.push_back(msg.str());
  };

  // Fast signature checks (C/C++-friendly: integers/pointers, optional double).
  for (const llvm::Argument &A : F.args()) {
    if (!isSupportedSymAbsValueType(A.getType(), opt)) {
      std::string s;
      llvm::raw_string_ostream os(s);
      os << "unsupported argument type: ";
      A.getType()->print(os);
      addIssue(os.str());
    }
  }
  if (!isSupportedSymAbsValueType(F.getReturnType(), opt)) {
    std::string s;
    llvm::raw_string_ostream os(s);
    os << "unsupported return type: ";
    F.getReturnType()->print(os);
    addIssue(os.str());
  }

  // Instruction-level checks.
  const llvm::DataLayout &DL = M.getDataLayout();
  const unsigned ptrBits = std::max(1u, DL.getPointerSizeInBits(0));

  for (const llvm::BasicBlock &BB : F) {
    for (const llvm::Instruction &I : BB) {
      if (isUnsupportedSymAbsInstruction(I)) {
        addIssue(llvm::Twine("unsupported instruction: ") +
                 llvm::Twine(llvm::Instruction::getOpcodeName(I.getOpcode())));
        continue;
      }

      // Types must be representable by SymAbsAI's Z3 encoding.
      if (!isSupportedSymAbsValueType(I.getType(), opt)) {
        std::string s;
        llvm::raw_string_ostream os(s);
        os << "unsupported instruction result type in " << I.getOpcodeName()
           << ": ";
        I.getType()->print(os);
        addIssue(os.str());
      }

      for (const llvm::Use &U : I.operands()) {
        llvm::Value *V = U.get();
        if (!V)
          continue;
        if (!isSupportedSymAbsValueType(V->getType(), opt)) {
          std::string s;
          llvm::raw_string_ostream os(s);
          os << "unsupported operand type used by " << I.getOpcodeName()
             << ": ";
          V->getType()->print(os);
          addIssue(os.str());
        }
      }

      // Floating-point casts are not supported (double-only model).
      if (llvm::isa<llvm::FPTruncInst>(I) || llvm::isa<llvm::FPExtInst>(I)) {
        addIssue(llvm::Twine("unsupported floating-point cast: ") +
                 llvm::Twine(llvm::Instruction::getOpcodeName(I.getOpcode())));
      }

      // Avoid known crash in getelementptr encoding when index bitwidth > ptr
      // bitwidth.
      if (const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
        for (const auto *idxIt = GEP->idx_begin(); idxIt != GEP->idx_end();
             ++idxIt) {
          llvm::Value *Idx = idxIt->get();
          if (!Idx)
            continue;
          llvm::Type *Ty = Idx->getType();
          if (Ty->isIntegerTy() && Ty->getIntegerBitWidth() > ptrBits) {
            addIssue("unsupported gep index width (> pointer width)");
            break;
          }
        }
      }

      // Stop early once we have enough context.
      if (issues.size() >= 20)
        break;
    }
    if (issues.size() >= 20)
      break;
  }

  if (issues.empty())
    return;

  std::string msg;
  llvm::raw_string_ostream os(msg);
  os << "SifaSymAbs: unsupported LLVM IR for function `" << F.getName()
     << "`.\n";
  os << "Supported subset: integers (<=64-bit), pointers";
  if (opt.allowDouble)
    os << ", and double";
  os << ".\n";
  os << "Issues (first " << issues.size() << "):\n";
  for (const auto &s : issues)
    os << "  - " << s << "\n";
  throw std::invalid_argument(os.str());
}

static symabs_ai::configparser::Config
makeConfig(const SifaSymAbsOptions &opt) {
  symabs_ai::configparser::Config cfg;
  cfg.set("ModuleContext", "Recursive", opt.recursive);
  cfg.set("Analyzer", "Variant", opt.analyzerVariant);
  cfg.set("AbstractDomain", "Variant", opt.abstractDomain);
  cfg.set("FunctionContext", "RepresentAllInstructions",
          opt.representAllInstructions);
  if (opt.wideningDelay >= 0)
    cfg.set("Analyzer", "WideningDelay", opt.wideningDelay);
  if (opt.wideningFrequency >= 0)
    cfg.set("Analyzer", "WideningFrequency", opt.wideningFrequency);
  return cfg;
}

static SymAbsState runForTarget(const llvm::Module &M, const llvm::Function &F,
                                llvm::BasicBlock *target,
                                const SifaSymAbsOptions &options) {
  const auto overallStart = std::chrono::steady_clock::now();

  auto stageStart = std::chrono::steady_clock::now();
  validateLlvmSubsetOrThrow(M, F, options);
  logProfileTiming("validateLlvmSubset",
                   std::chrono::steady_clock::now() - stageStart);

  // Configure SifaLogger from options.
  SifaLogLevel level = options.logLevel;
  if (options.progressStream && level == SifaLogLevel::None)
    level = SifaLogLevel::Progress;
  if (level != SifaLogLevel::None) {
    SifaLogger::setOutputStream(options.progressStream ? options.progressStream
                                                       : &llvm::errs());
    SifaLogger::setLevel(level);
  }

  symabs_ai::Analyzer::resetBestTransformerCallCount();
  symabs_ai::Analyzer::resetSmtSolverCallCount();
  auto cfg = makeConfig(options);

  // SymAbsAI expects non-const pointers (it mutates analysis state
  // and queries IR properties through non-const APIs).
  auto *mod = const_cast<llvm::Module *>(&M);
  auto *fun = const_cast<llvm::Function *>(&F);

  SifaLogger::progress("Building module context...");
  stageStart = std::chrono::steady_clock::now();
  symabs_ai::ModuleContext mctx(mod, cfg);
  logProfileTiming("ModuleContext",
                   std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress("Building function context and analyzer...");
  stageStart = std::chrono::steady_clock::now();
  auto fctxPtr = mctx.createFunctionContext(fun);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctxPtr);
  const auto fcfg = fctxPtr->getConfig();
  symabs_ai::DomainConstructor dom(fcfg);
  auto analyzer = symabs_ai::Analyzer::New(*fctxPtr, fragDecomp, dom);
  logProfileTiming("FunctionContext+Analyzer",
                   std::chrono::steady_clock::now() - stageStart);

  SifaStats stats;
  SifaSymAbsDomain domain(*fctxPtr, dom, *analyzer);
  NeverFluid<SymAbsState> fluid;

  DagInterpreter<Transition, SymAbsState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, SymAbsState> loopSum(stats, domain, fluid,
                                                          ipr);
  ipr.setLoopSummarizer(loopSum);

  SifaLogger::progress("Building procedure resources (regex DAG)...");
  stageStart = std::chrono::steady_clock::now();
  ProcedureResources res(stats, *fun, {target});
  logProfileTiming("ProcedureResources",
                   std::chrono::steady_clock::now() - stageStart);
  auto initial = domain.makeTopAt(&fun->getEntryBlock(), /*after=*/false);

  SifaLogger::progress("Running fixpoint interpretation...");
  stageStart = std::chrono::steady_clock::now();
  // Interpret for the unique marker in the LOI overlay.
  SymAbsState out = ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToLois(), initial);
  logProfileTiming("Interpret", std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress(
      "bestTransformer calls: " +
      std::to_string(symabs_ai::Analyzer::getBestTransformerCallCount()) +
      ", SMT solver calls: " +
      std::to_string(symabs_ai::Analyzer::getSmtSolverCallCount()));
  if (profileEnabled()) {
    llvm::errs() << "[sifa-profile] bestTransformer calls: "
                 << symabs_ai::Analyzer::getBestTransformerCallCount()
                 << ", SMT solver calls: "
                 << symabs_ai::Analyzer::getSmtSolverCallCount() << "\n";
    logProfileTiming("Total", std::chrono::steady_clock::now() - overallStart);
  }
  return out;
}

static SymAbsState runForReturn(const llvm::Module &M, const llvm::Function &F,
                                const SifaSymAbsOptions &options) {
  const auto overallStart = std::chrono::steady_clock::now();

  auto stageStart = std::chrono::steady_clock::now();
  validateLlvmSubsetOrThrow(M, F, options);
  logProfileTiming("validateLlvmSubset",
                   std::chrono::steady_clock::now() - stageStart);

  // Configure SifaLogger from options.
  SifaLogLevel level = options.logLevel;
  if (options.progressStream && level == SifaLogLevel::None)
    level = SifaLogLevel::Progress;
  if (level != SifaLogLevel::None) {
    SifaLogger::setOutputStream(options.progressStream ? options.progressStream
                                                       : &llvm::errs());
    SifaLogger::setLevel(level);
  }

  symabs_ai::Analyzer::resetBestTransformerCallCount();
  symabs_ai::Analyzer::resetSmtSolverCallCount();
  auto cfg = makeConfig(options);

  auto *mod = const_cast<llvm::Module *>(&M);
  auto *fun = const_cast<llvm::Function *>(&F);

  SifaLogger::progress("Building module context...");
  stageStart = std::chrono::steady_clock::now();
  symabs_ai::ModuleContext mctx(mod, cfg);
  logProfileTiming("ModuleContext",
                   std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress("Building function context and analyzer...");
  stageStart = std::chrono::steady_clock::now();
  auto fctxPtr = mctx.createFunctionContext(fun);
  auto fragDecomp = symabs_ai::FragmentDecomposition::For(*fctxPtr);
  const auto fcfg = fctxPtr->getConfig();
  symabs_ai::DomainConstructor dom(fcfg);
  auto analyzer = symabs_ai::Analyzer::New(*fctxPtr, fragDecomp, dom);
  logProfileTiming("FunctionContext+Analyzer",
                   std::chrono::steady_clock::now() - stageStart);

  SifaStats stats;
  SifaSymAbsDomain domain(*fctxPtr, dom, *analyzer);
  NeverFluid<SymAbsState> fluid;

  DagInterpreter<Transition, SymAbsState> ipr(stats, domain, fluid);
  FixpointLoopSummarizer<Transition, SymAbsState> loopSum(stats, domain, fluid,
                                                          ipr);
  ipr.setLoopSummarizer(loopSum);

  SifaLogger::progress("Building procedure resources (regex DAG)...");
  stageStart = std::chrono::steady_clock::now();
  // No LOIs needed; ProcedureResources always adds an EXIT marker. This path
  // remains intraprocedural: call semantics come from SymAbsAI's
  // own transformers rather than Sifa's call-summary engine.
  ProcedureResources res(stats, *fun, std::vector<llvm::BasicBlock *>{});
  logProfileTiming("ProcedureResources",
                   std::chrono::steady_clock::now() - stageStart);
  auto initial = domain.makeTopAt(&fun->getEntryBlock(), /*after=*/false);

  SifaLogger::progress("Running fixpoint interpretation...");
  stageStart = std::chrono::steady_clock::now();
  SymAbsState out = ipr.interpretForSingleMarker(
      res.getRegexDag(), res.getDagOverlayPathToReturn(), initial);
  logProfileTiming("Interpret", std::chrono::steady_clock::now() - stageStart);
  SifaLogger::progress(
      "bestTransformer calls: " +
      std::to_string(symabs_ai::Analyzer::getBestTransformerCallCount()) +
      ", SMT solver calls: " +
      std::to_string(symabs_ai::Analyzer::getSmtSolverCallCount()));
  if (profileEnabled()) {
    llvm::errs() << "[sifa-profile] bestTransformer calls: "
                 << symabs_ai::Analyzer::getBestTransformerCallCount()
                 << ", SMT solver calls: "
                 << symabs_ai::Analyzer::getSmtSolverCallCount() << "\n";
    logProfileTiming("Total", std::chrono::steady_clock::now() - overallStart);
  }
  return out;
}

SymAbsState lotus::sifa::analyzeSymAbsTo(const llvm::Module &M,
                                         const llvm::Function &F,
                                         const llvm::BasicBlock &target,
                                         const SifaSymAbsOptions &options) {
  return runForTarget(M, F, const_cast<llvm::BasicBlock *>(&target), options);
}

bool lotus::sifa::isReachableSymAbs(const llvm::Module &M,
                                    const llvm::Function &F,
                                    const llvm::BasicBlock &target,
                                    const SifaSymAbsOptions &options) {
  const SymAbsState out = analyzeSymAbsTo(M, F, target, options);
  return out && !out->isBottom();
}

SymAbsState
lotus::sifa::analyzeSymAbsToReturn(const llvm::Module &M,
                                   const llvm::Function &F,
                                   const SifaSymAbsOptions &options) {
  return runForReturn(M, F, options);
}

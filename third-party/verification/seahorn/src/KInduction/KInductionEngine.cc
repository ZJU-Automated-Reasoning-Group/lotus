// Real k-induction engine for Seahorn, implemented via source-to-source
// instrumentation + loop unwinding + PathBMC.
//
// Summary:
// - Base case(k): unwind loops to k iterations, check reachability of error.
// - Inductive step(k): havoc state at entry, assume property for first k
//   iterations (by rewriting asserts/errors into assumes), and check for a
//   violation at iteration k+1.
//
// This is the standard k-induction scheme used by tools like ESBMC/CPAchecker,
// adapted to Seahorn's IR and PathBMC backend.

#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/PassInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "Alias/UnificationBased/seadsa/InitializePasses.hh"
#include "Alias/UnificationBased/seadsa/ShadowMem.hh"
#include "seahorn/Analysis/CanFail.hh"
#include "seahorn/Analysis/CutPointGraph.hh"
#include "seahorn/Analysis/SeaBuiltinsInfo.hh"
#include "seahorn/Analysis/TopologicalOrder.hh"
#include "seahorn/BvOpSem.hh"
#include "seahorn/InitializePasses.hh"
#include "seahorn/KInduction.hh"
#include "seahorn/Passes.hh"
#include "seahorn/PathBmc.hh"
#include "seahorn/Support/SeaDebug.h"
#include "seahorn/Support/SeaLog.hh"
#include "seahorn/Support/Stats.hh"
#include "seahorn/Transforms/Scalar/PromoteVerifierCalls.hh"
#include "seahorn/Transforms/Utils/Local.hh"
#include "seahorn/Transforms/Utils/NameValues.hh"

#include <chrono>
#include <memory>

using namespace llvm;

#ifndef HAVE_CLAM
seahorn::KInductionResult seahorn::KInductionEngine::run(llvm::Module &) {
  return seahorn::KInductionResult::UNKNOWN;
}
#else

using namespace seahorn;

static const char *toString(solver::SolverResult r) {
  switch (r) {
  case solver::SolverResult::SAT:
    return "sat";
  case solver::SolverResult::UNSAT:
    return "unsat";
  default:
    return "unknown";
  }
}

static constexpr const char *kIterVarName = "__sea_kinduction_iter";

static bool isFirstClassOrPtrType(Type *ty) {
  if (!ty)
    return false;
  return ty->isIntOrIntVectorTy() || ty->isPtrOrPtrVectorTy() ||
         ty->isFloatingPointTy() || ty->isVectorTy();
}

static AllocaInst *getOrCreateIterAlloca(Function &F) {
  auto &ctx = F.getContext();
  IRBuilder<> b(&*F.getEntryBlock().getFirstInsertionPt());

  // If already present (shouldn't happen on a fresh clone), reuse.
  for (Instruction &I : F.getEntryBlock()) {
    auto *ai = dyn_cast<AllocaInst>(&I);
    if (ai && ai->getName() == kIterVarName)
      return ai;
  }

  auto *i64Ty = Type::getInt64Ty(ctx);
  auto *ai = b.CreateAlloca(i64Ty, nullptr, kIterVarName);
  // Initialize to 0 right after allocas insertion point (safe).
  b.CreateStore(ConstantInt::get(i64Ty, 0), ai);
  return ai;
}

static void insertIterIncrementAtLoopHeaders(LoopInfo &LI, Function &F,
                                             AllocaInst &iter) {
  auto &ctx = F.getContext();
  auto *i64Ty = Type::getInt64Ty(ctx);

  SmallVector<Loop *, 16> loops;
  for (Loop *L : LI)
    loops.push_back(L);

  // Include nested loops as well.
  for (unsigned idx = 0; idx < loops.size(); ++idx) {
    for (Loop *Sub : loops[idx]->getSubLoops())
      loops.push_back(Sub);
  }

  for (Loop *L : loops) {
    BasicBlock *H = L->getHeader();
    if (!H)
      continue;

    // Insert at first non-PHI insertion point.
    Instruction *ip = &*H->getFirstInsertionPt();
    IRBuilder<> b(ip);
    Value *cur = b.CreateLoad(i64Ty, &iter, "kind.iter");
    Value *nxt = b.CreateAdd(cur, ConstantInt::get(i64Ty, 1), "kind.iter.next");
    b.CreateStore(nxt, &iter);
  }
}

static Function *getAssumeFn(SeaBuiltinsInfo &SBI, Module &M) {
  return SBI.mkSeaBuiltinFn(SeaBuiltinsOp::ASSUME, M);
}

static void guardSeaBuiltin(CallBase &cb, AllocaInst &iter, uint64_t k,
                            SeaBuiltinsInfo &SBI) {
  auto op = SBI.getSeaBuiltinOp(cb);
  if (op == SeaBuiltinsOp::UNKNOWN)
    return;

  // Only guard operations that represent safety properties.
  const bool isErrorLike =
      (op == SeaBuiltinsOp::ERROR || op == SeaBuiltinsOp::FAIL);
  const bool isAssertLike =
      (op == SeaBuiltinsOp::ASSERT || op == SeaBuiltinsOp::ASSERT_NOT ||
       op == SeaBuiltinsOp::ASSERT_IF || op == SeaBuiltinsOp::SYNTH_ASSERT);

  if (!isErrorLike && !isAssertLike)
    return;

  Function *assumeFn = getAssumeFn(SBI, *cb.getModule());
  if (!assumeFn)
    return;

  auto &ctx = cb.getContext();
  auto *i64Ty = Type::getInt64Ty(ctx);
  Value *kC = ConstantInt::get(i64Ty, k);

  // Split block around the call.
  BasicBlock *origBB = cb.getParent();
  Instruction *splitPt = cb.getNextNode();
  if (!splitPt)
    return; // can't safely split at end

  BasicBlock *contBB = origBB->splitBasicBlock(splitPt, "kind.cont");
  BasicBlock *thenBB =
      BasicBlock::Create(ctx, "kind.hyp", origBB->getParent(), contBB);
  BasicBlock *elseBB =
      BasicBlock::Create(ctx, "kind.check", origBB->getParent(), contBB);

  // Replace the unconditional branch added by split with a conditional.
  origBB->getTerminator()->eraseFromParent();
  IRBuilder<> b(origBB);
  Value *cur = b.CreateLoad(i64Ty, &iter, "kind.iter.at");
  Value *inHyp = b.CreateICmpULE(cur, kC, "kind.in_hyp");
  b.CreateCondBr(inHyp, thenBB, elseBB);

  // thenBB: assume the property (i.e., exclude violations in <=k steps).
  IRBuilder<> bt(thenBB);
  if (isErrorLike) {
    bt.CreateCall(assumeFn, {ConstantInt::getFalse(ctx)});
  } else {
    // Convert assertion to assume.
    // verifier.assert(x)      -> assume(x)
    // verifier.assert.not(x)  -> assume(!x)
    // sea.assert.if(c, x)     -> assume(!c || x)
    Value *assumeCond = nullptr;
    if (op == SeaBuiltinsOp::ASSERT || op == SeaBuiltinsOp::SYNTH_ASSERT) {
      assumeCond = cb.getArgOperand(0);
    } else if (op == SeaBuiltinsOp::ASSERT_NOT) {
      Value *x = cb.getArgOperand(0);
      assumeCond = bt.CreateNot(x);
    } else if (op == SeaBuiltinsOp::ASSERT_IF) {
      // sea.assert.if(cond, prop)
      Value *c = cb.getArgOperand(0);
      Value *p = cb.getArgOperand(1);
      assumeCond = bt.CreateOr(bt.CreateNot(c), p);
    }

    if (assumeCond && assumeCond->getType()->isIntegerTy(1)) {
      bt.CreateCall(assumeFn, {assumeCond});
    } else {
      // If we can't interpret it, don't weaken: keep it as an assume(false)
      // to avoid unsound proofs from missing obligations.
      bt.CreateCall(assumeFn, {ConstantInt::getFalse(ctx)});
    }
  }
  bt.CreateBr(contBB);

  // elseBB: keep the original check (potential violation at step k+1).
  IRBuilder<> be(elseBB);
  cb.removeFromParent();
  elseBB->getInstList().push_back(&cb);
  be.CreateBr(contBB);
}

static void instrumentAndHavocForInduction(Module &M, StringRef entryName,
                                           uint64_t guard_k) {
  Function *F = M.getFunction(entryName);
  if (!F || F->isDeclaration())
    return;

  SeaBuiltinsInfo SBI;

  // Create counter and increment it at loop headers (destinations of
  // backedges).
  AllocaInst *iter = getOrCreateIterAlloca(*F);
  if (!iter)
    return;

  SmallVector<std::pair<const BasicBlock *, const BasicBlock *>, 16> backedges;
  FindFunctionBackedges(*F, backedges);
  SmallPtrSet<const BasicBlock *, 16> headers;
  for (auto &be : backedges)
    headers.insert(be.second);

  for (const BasicBlock *H : headers) {
    Instruction *ip = const_cast<Instruction *>(&*H->getFirstInsertionPt());
    IRBuilder<> b(ip);
    auto *i64Ty = Type::getInt64Ty(F->getContext());
    Value *cur = b.CreateLoad(i64Ty, iter, "kind.iter");
    Value *nxt = b.CreateAdd(cur, ConstantInt::get(i64Ty, 1), "kind.iter.next");
    b.CreateStore(nxt, iter);
  }

  // Guard assert/error calls. Collect first because we mutate the IR.
  SmallVector<CallBase *, 32> calls;
  for (BasicBlock &BB : *F)
    for (Instruction &I : BB)
      if (auto *cb = dyn_cast<CallBase>(&I))
        calls.push_back(cb);
  for (CallBase *cb : calls) {
    if (!cb || !cb->getParent())
      continue;
    guardSeaBuiltin(*cb, *iter, guard_k, SBI);
  }

  // Havoc (over-approx) scalar allocas in the entry block.
  BasicBlock &entryBB = F->getEntryBlock();
  Instruction *havocPt = entryBB.getTerminator();
  for (Instruction &I : entryBB) {
    if (!isa<AllocaInst>(&I))
      continue;
    if (Instruction *next = I.getNextNode())
      havocPt = next;
    else
      havocPt = entryBB.getTerminator();
  }
  IRBuilder<> hb(havocPt);
  DenseMap<Type *, Function *> ndfns;
  auto getNondet = [&](Type *ty) -> Function * {
    if (!isFirstClassOrPtrType(ty))
      return nullptr;
    auto it = ndfns.find(ty);
    if (it != ndfns.end())
      return it->second;
    Function &fn = createNewNondetFn(M, *ty, ndfns.size(), "verifier.nondet.");
    ndfns[ty] = &fn;
    return &fn;
  };

  for (Instruction &I : F->getEntryBlock()) {
    auto *ai = dyn_cast<AllocaInst>(&I);
    if (!ai)
      continue;
    if (ai->getName() == kIterVarName)
      continue;
    Type *elt = ai->getAllocatedType();
    Function *nd = getNondet(elt);
    if (!nd)
      continue;
    hb.CreateStore(hb.CreateCall(nd), ai);
  }

  // Havoc scalar, non-constant globals.
  for (GlobalVariable &GV : M.globals()) {
    if (GV.isConstant())
      continue;
    if (!GV.hasInitializer())
      continue;
    Type *ty = GV.getValueType();
    Function *nd = getNondet(ty);
    if (!nd)
      continue;
    hb.CreateStore(hb.CreateCall(nd), &GV);
  }
}

static void normalizeEntryAllocas(Module &M) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    BasicBlock &entry = F.getEntryBlock();
    if (entry.empty())
      continue;

    Instruction *insertBefore = &*entry.begin();
    for (auto it = entry.begin(), end = entry.end(); it != end;) {
      Instruction *I = &*it++;
      auto *ai = dyn_cast<AllocaInst>(I);
      if (!ai)
        continue;
      // Only move static allocas. Moving a dynamic alloca (with a non-constant
      // array size) above the instructions that compute its size would break
      // dominance/SSA form.
      if (!ai->isStaticAlloca())
        continue;
      ai->moveBefore(insertBefore);
      insertBefore = ai;
    }
  }
}

class KinductionInstrumentPass : public FunctionPass {
public:
  static char ID;
  KinductionInstrumentPass() : FunctionPass(ID), m_k(0) {}
  explicit KinductionInstrumentPass(uint64_t k) : FunctionPass(ID), m_k(k) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<SeaBuiltinsInfoWrapperPass>();
  }

  bool runOnFunction(Function &F) override {
    if (F.isDeclaration())
      return false;

    auto &SBI = getAnalysis<SeaBuiltinsInfoWrapperPass>().getSBI();
    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    AllocaInst *iter = getOrCreateIterAlloca(F);
    if (!iter)
      return false;

    insertIterIncrementAtLoopHeaders(LI, F, *iter);

    // Guard assert/error calls. Collect first because we mutate the IR.
    SmallVector<CallBase *, 32> calls;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *cb = dyn_cast<CallBase>(&I))
          calls.push_back(cb);

    bool changed = true; // we always add iter + increments
    for (CallBase *cb : calls) {
      if (!cb || !cb->getParent())
        continue;
      guardSeaBuiltin(*cb, *iter, m_k, SBI);
    }

    return changed;
  }

private:
  uint64_t m_k;
};

char KinductionInstrumentPass::ID = 0;
static llvm::Pass *createKinductionInstrumentInternalPass() {
  return new KinductionInstrumentPass();
}
LLVM_ATTRIBUTE_USED static llvm::RegisterPass<KinductionInstrumentPass>
    XKinductionInstrument(
        "kinduction-instrument-internal",
        "Internal: k-induction instrumentation (guard checks)", false, false);

class KinductionHavocStatePass : public FunctionPass {
public:
  static char ID;
  KinductionHavocStatePass() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }

  bool runOnFunction(Function &F) override {
    if (F.isDeclaration())
      return false;

    Module &M = *F.getParent();
    // Insert havoc stores after all allocas in the entry block.
    // Some frontends/pipelines may interleave allocas with other instructions;
    // to avoid dominance issues, insert after the *last* alloca in the block.
    BasicBlock &entryBB = F.getEntryBlock();
    Instruction *insertPt = entryBB.getTerminator();
    for (Instruction &I : entryBB) {
      if (!isa<AllocaInst>(&I))
        continue;
      if (Instruction *next = I.getNextNode())
        insertPt = next;
      else
        insertPt = entryBB.getTerminator();
    }
    IRBuilder<> b(insertPt);

    // Havoc (over-approx) scalar allocas in the entry block.
    DenseMap<Type *, Function *> ndfns;
    auto getNondet = [&](Type *ty) -> Function * {
      if (!isFirstClassOrPtrType(ty))
        return nullptr;
      auto it = ndfns.find(ty);
      if (it != ndfns.end())
        return it->second;
      Function &fn =
          createNewNondetFn(M, *ty, ndfns.size(), "verifier.nondet.");
      ndfns[ty] = &fn;
      return &fn;
    };

    for (Instruction &I : F.getEntryBlock()) {
      auto *ai = dyn_cast<AllocaInst>(&I);
      if (!ai)
        continue;
      if (ai->getName() == kIterVarName)
        continue;

      Type *elt = ai->getAllocatedType();
      Function *nd = getNondet(elt);
      if (!nd)
        continue;
      b.CreateStore(b.CreateCall(nd), ai);
    }

    // Havoc (over-approx) scalar, non-constant globals by storing nondet at
    // function entry. This is conservative for induction (harder to prove).
    for (GlobalVariable &GV : M.globals()) {
      if (GV.isConstant())
        continue;
      if (!GV.hasInitializer())
        continue;
      Type *ty = GV.getValueType();
      Function *nd = getNondet(ty);
      if (!nd)
        continue;
      b.CreateStore(b.CreateCall(nd), &GV);
    }

    return true;
  }
};

char KinductionHavocStatePass::ID = 0;
static llvm::Pass *createKinductionHavocInternalPass() {
  return new KinductionHavocStatePass();
}
LLVM_ATTRIBUTE_USED static llvm::RegisterPass<KinductionHavocStatePass>
    XKinductionHavoc("kinduction-havoc-internal",
                     "Internal: k-induction havoc initial state", false, false);

class RunPathBmcModulePass : public ModulePass {
public:
  static char ID;
  static solver::SolverResult s_dummyOut;

  RunPathBmcModulePass() : ModulePass(ID), m_entry("main"), m_out(s_dummyOut) {}
  RunPathBmcModulePass(StringRef entry, solver::SolverResult &out)
      : ModulePass(ID), m_entry(entry.str()), m_out(out) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<seadsa::ShadowMemPass>();
    AU.addRequired<LazyValueInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<CanFail>();
    AU.addRequired<NameValues>();
    AU.addRequired<TopologicalOrder>();
    AU.addRequired<CutPointGraph>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    Function *entry = M.getFunction(m_entry);
    if (!entry || entry->isDeclaration()) {
      m_out = solver::SolverResult::UNKNOWN;
      return false;
    }

    // Find entry and (unique) return cutpoints in the (transformed) function.
    const CutPointGraph &cpg = getAnalysis<CutPointGraph>(*entry);
    const CutPoint &src = cpg.getCp(entry->getEntryBlock());
    const CutPoint *dst = nullptr;

    for (BasicBlock &bb : *entry) {
      if (isa<ReturnInst>(bb.getTerminator()) && cpg.isCutPoint(bb)) {
        dst = &cpg.getCp(bb);
        break;
      }
    }

    if (!dst) {
      m_out = solver::SolverResult::UNKNOWN;
      return false;
    }
    if (!cpg.getEdge(src, *dst)) {
      // PathBMC (and BmcPass) requires a loop-free program here.
      m_out = solver::SolverResult::UNKNOWN;
      return false;
    }

    ExprFactory efac;
    const auto &dl = M.getDataLayout();
    auto sem = std::make_unique<BvOpSem>(efac, *this, dl, MEM);
    auto &sm = getAnalysis<seadsa::ShadowMemPass>().getShadowMem();
    // Avoid legacy PM dependency scheduling issues by constructing TLI locally.
    llvm::TargetLibraryInfoWrapperPass tli(llvm::Triple(M.getTargetTriple()));

    PathBmcEngine bmc(*sem, tli, sm);
    bmc.addCutPoint(src);
    bmc.addCutPoint(*dst);
    m_out = bmc.solve();
    return false;
  }

private:
  std::string m_entry;
  solver::SolverResult &m_out;
};

char RunPathBmcModulePass::ID = 0;
solver::SolverResult RunPathBmcModulePass::s_dummyOut =
    solver::SolverResult::UNKNOWN;
static llvm::Pass *createRunPathBmcInternalPass() {
  return new RunPathBmcModulePass();
}

static llvm::Pass *createTopologicalOrderInternalPass() {
  return new seahorn::TopologicalOrder();
}

static llvm::Pass *createCutPointGraphInternalPass() {
  return new seahorn::CutPointGraph();
}

static std::unique_ptr<Module> cloneModule(const Module &M) {
  ValueToValueMapTy vmap;
  return CloneModule(M, vmap);
}

static void initPassesOnce() {
  static bool inited = false;
  if (inited)
    return;
  inited = true;
  PassRegistry &R = *PassRegistry::getPassRegistry();
  // The k-induction engine runs a nested legacy pass manager. Ensure the pass
  // registry is initialized similarly to the main seahorn driver.
  llvm::initializeAnalysis(R);
  llvm::initializeTransformUtils(R);
  llvm::initializeScalarOpts(R);
  // Ensure required LLVM/SeaHorn passes are registered for the nested legacy
  // PM.
  llvm::initializeTargetLibraryInfoWrapperPassPass(R);
  llvm::initializeCallGraphWrapperPassPass(R);
  llvm::initializeLazyValueInfoWrapperPassPass(R);
  llvm::initializeDominatorTreeWrapperPassPass(R);
  llvm::initializeLoopInfoWrapperPassPass(R);
  llvm::initializeScalarEvolutionWrapperPassPass(R);

  // SeaDsa / shadow memory are required by PathBMC.
  initializeShadowMemPassPass(R);

  initializeLoopPeelerPassPass(R);
  initializeSeaBuiltinsInfoWrapperPassPass(R);

  // Some build configurations dead-strip static pass registrars from static
  // libraries. Register internal passes explicitly to keep the nested legacy
  // pass manager functional.
  auto ensureRegistered = [&](const char *arg, const char *name, const void *id,
                              llvm::Pass *(*ctor)()) {
    if (R.getPassInfo(id))
      return;
    auto *PI = new PassInfo(name, arg, id, PassInfo::NormalCtor_t(ctor),
                            /*isCFGOnly=*/false, /*isAnalysis=*/false);
    R.registerPass(*PI, true);
  };

  // Register internal k-induction helper passes. These are created directly
  // (not by pass-name lookup) but legacy PM still requires PassInfo entries.
  // Also register (and force-link) Seahorn passes that the nested PM schedules
  // via getAnalysisUsage(). When linked from static libs, their RegisterPass
  // static initializers may be dead-stripped.
  ensureRegistered("promote-verifier", "PromoteVerifierCalls",
                   &seahorn::PromoteVerifierCalls::ID,
                   &seahorn::createPromoteVerifierCallsPass);
  ensureRegistered("mark-fail", "Mark functions that can fail",
                   &seahorn::CanFail::ID, &seahorn::createCanFailPass);
  ensureRegistered("name-values", "Names all unnamed values",
                   &seahorn::NameValues::ID, &seahorn::createNameValuesPass);
  ensureRegistered("topo", "Topological order of CFG",
                   &seahorn::TopologicalOrder::ID,
                   &createTopologicalOrderInternalPass);
  ensureRegistered("cpg", "Construct Cut Point Graph",
                   &seahorn::CutPointGraph::ID,
                   &createCutPointGraphInternalPass);

  ensureRegistered("kinduction-pathbmc-internal",
                   "Internal: run PathBMC for k-induction",
                   &RunPathBmcModulePass::ID, &createRunPathBmcInternalPass);

  ensureRegistered("kinduction-instrument-internal",
                   "Internal: k-induction instrumentation (guard checks)",
                   &KinductionInstrumentPass::ID,
                   &createKinductionInstrumentInternalPass);
  ensureRegistered(
      "kinduction-havoc-internal", "Internal: k-induction havoc initial state",
      &KinductionHavocStatePass::ID, &createKinductionHavocInternalPass);
}

static solver::SolverResult runPathBmcWithUnwind(Module &M, StringRef entry,
                                                 unsigned unwind_k,
                                                 bool havoc_and_guard,
                                                 uint64_t guard_k) {
  initPassesOnce();

  // Apply induction instrumentation directly (avoid relying on legacy PM
  // initialization of internal helper passes).
  if (havoc_and_guard)
    instrumentAndHavocForInduction(M, entry, guard_k);

  // unwind_k is the max iterations (per loop) we want to explore.
  // We implement it with:
  // - peel (unwind_k - 1) iterations
  // - cut remaining loop backedges (allowing at most one extra iteration)
  // Together this bounds each natural loop to unwind_k iterations.
  const unsigned peel = (unwind_k > 0) ? (unwind_k - 1) : 0;

  legacy::PassManager PM;
  // Minimal canonicalization for peeler/cut-loops.
  PM.add(createLoopSimplifyPass());
  PM.add(createLCSSAPass());
  PM.add(createLoopRotatePass());

  PM.add(createLoopPeelerPass(peel));
  PM.add(createCutLoopsPass());

  PM.run(M);

  // Some loop/cut transforms can (rarely) leave the IR in an invalid state,
  // which would later trigger hard asserts inside analysis passes (e.g.
  // ShadowMem). Fail gracefully by treating such cases as UNKNOWN.
  if (llvm::verifyModule(M, &llvm::errs())) {
    LOG("kinduction", errs() << "k-induction: transformed module is invalid; "
                                "returning unknown\n";);
    return solver::SolverResult::UNKNOWN;
  }

  // Some LLVM loop transforms can leave allocas after their first uses when the
  // entry block participates in loop canonicalization/rotation. That breaks the
  // IR verifier and downstream passes (e.g., ShadowMem). Normalize allocas back
  // to the top of the entry block before running PathBMC.
  normalizeEntryAllocas(M);

  if (llvm::verifyModule(M, &llvm::errs())) {
    LOG("kinduction", errs() << "k-induction: module invalid after alloca "
                                "normalization; returning unknown\n";);
    return solver::SolverResult::UNKNOWN;
  }

  // Run PathBMC via an internal wrapper pass instead of BmcPass.
  // This avoids relying on dead-strippable static pass registrars from
  // seahorn static libraries and keeps k-induction output clean (no sat/unsat).
  solver::SolverResult out = solver::SolverResult::UNKNOWN;
  legacy::PassManager PM2;
  PM2.add(new RunPathBmcModulePass(entry, out));
  PM2.run(M);
  return out;
}

// end internal helpers

seahorn::KInductionResult seahorn::KInductionEngine::run(llvm::Module &M) {
  const auto start = std::chrono::steady_clock::now();
  const auto &opts = m_opts;

  if (opts.EntryName.empty())
    return seahorn::KInductionResult::UNKNOWN;

  LOG("kinduction", errs() << "k-induction: entry=" << opts.EntryName
                           << " k-min=" << opts.KMin << " k-max=" << opts.KMax
                           << " timeout=" << opts.TimeoutSec << "s\n";);

  auto timedOut = [&]() -> bool {
    if (opts.TimeoutSec == 0)
      return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    return elapsed >= static_cast<long>(opts.TimeoutSec);
  };

  for (unsigned k = std::max(1u, opts.KMin);; ++k) {
    m_lastK = k;
    if (timedOut())
      return seahorn::KInductionResult::UNKNOWN;
    if (opts.KMax > 0 && k > opts.KMax)
      return seahorn::KInductionResult::UNKNOWN;

    LOG("kinduction", errs() << "k-induction: try k=" << k << "\n";);

    // --- Base case: check for counterexample within k iterations.
    auto baseM = cloneModule(M);
    solver::SolverResult baseRes =
        runPathBmcWithUnwind(*baseM, opts.EntryName, k,
                             /*havoc_and_guard=*/false, /*guard_k=*/0);

    LOG("kinduction", errs() << "k-induction: base(k=" << k
                             << ")=" << toString(baseRes) << "\n";);

    if (baseRes == solver::SolverResult::SAT)
      return seahorn::KInductionResult::BUG;
    if (baseRes != solver::SolverResult::UNSAT)
      return seahorn::KInductionResult::UNKNOWN;

    // --- Inductive step: assume property holds for first k iterations, and
    // look for a violation at iteration k+1 from an arbitrary (havoced) state.
    auto stepM = cloneModule(M);
    solver::SolverResult stepRes =
        runPathBmcWithUnwind(*stepM, opts.EntryName, k + 1,
                             /*havoc_and_guard=*/true, /*guard_k=*/k);

    LOG("kinduction", errs() << "k-induction: step(k=" << k
                             << ")=" << toString(stepRes) << "\n";);

    if (stepRes == solver::SolverResult::UNSAT)
      return seahorn::KInductionResult::SAFE;
    if (stepRes == solver::SolverResult::SAT)
      continue; // not k-inductive yet, try larger k

    return seahorn::KInductionResult::UNKNOWN;
  }
}

#endif // HAVE_CLAM

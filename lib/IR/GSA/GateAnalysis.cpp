//===- GateAnalysis.cpp - Read-only Gated SSA construction ---------------===//
//
// Builds immutable Gated SSA (GSA) expressions for existing PHI nodes without
// mutating LLVM IR. Materialization into selects is handled by a separate pass.
//
// The implementation is adapted from Havlak's construction of Thinned Gated
// Single-Assignment form, LCPC'93.
//
//===----------------------------------------------------------------------===//

#include "IR/GSA/GSA.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

#include <set>
#include <utility>
#include <vector>

using namespace llvm;

namespace gsa {

GateExpr::GateExpr(Type *type, Value *leaf_value)
    : m_kind(Kind::LeafValue), m_type(type), m_leaf_value(leaf_value) {
  assert(type && "Leaf expressions require a type");
  assert(leaf_value && "Leaf expressions require a value");
}

GateExpr::GateExpr(Type *type) : m_kind(Kind::Bottom), m_type(type) {
  assert(type && "Bottom expressions require a type");
}

GateExpr::GateExpr(Type *type, GateGuard true_guard, GateGuard false_guard,
                   const GateExpr *true_expr, const GateExpr *false_expr)
    : m_kind(Kind::Select), m_type(type), m_true_guard(std::move(true_guard)),
      m_false_guard(std::move(false_guard)), m_true_expr(true_expr),
      m_false_expr(false_expr) {
  assert(type && "Select expressions require a type");
  assert(true_expr && false_expr && "Select expressions require both arms");
}

GateExpr::GateExpr(Type *type, std::vector<SwitchArm> switch_arms)
    : m_kind(Kind::Switch), m_type(type), m_switch_arms(std::move(switch_arms)) {
  assert(type && "Switch expressions require a type");
}

static bool dominatesForUse(Value *V, Instruction *UseI, DominatorTree &DT) {
  if (!UseI)
    return false;
  if (isa<Argument>(V) || isa<Constant>(V))
    return true;
  auto *Inst = dyn_cast<Instruction>(V);
  if (!Inst)
    return false;
  if (Inst->getParent() == UseI->getParent())
    return Inst->comesBefore(UseI);
  return DT.dominates(Inst, UseI);
}

static cl::opt<bool> ThinnedGsa("gsa-thinned",
                                cl::desc("Emit thin gamma nodes (TGSA)"),
                                cl::init(true), cl::Hidden);

namespace {

class GateAnalysisImpl final : public GateAnalysis {
public:
  GateAnalysisImpl(Function &F, DominatorTree &DT, PostDominatorTree &PDT,
                   LoopInfo &LI, ControlDependenceAnalysis &CDA,
                   bool is_thinned)
      : m_function(F), m_DT(DT), m_PDT(PDT), m_LI(LI), m_CDA(CDA),
        m_is_thinned(is_thinned) {
    calculate();
  }

  bool hasGate(const PHINode &PN) const override {
    return m_gate_map.count(&PN) > 0;
  }

  const GateNode &getGate(const PHINode &PN) const override {
    auto It = m_gate_map.find(&PN);
    assert(It != m_gate_map.end() && "Unknown GSA gate");
    return *It->second;
  }

  ArrayRef<const GateNode *> gates() const override { return m_gate_list; }

  bool isThinned() const override { return m_is_thinned; }

private:
  using FlowMap = DenseMap<BasicBlock *, const GateExpr *>;

  Function &m_function;
  DominatorTree &m_DT;
  PostDominatorTree &m_PDT;
  LoopInfo &m_LI;
  ControlDependenceAnalysis &m_CDA;
  bool m_is_thinned{true};

  std::vector<std::unique_ptr<GateExpr>> m_owned_exprs;
  std::vector<std::unique_ptr<GateNode>> m_owned_gates;
  DenseMap<const PHINode *, const GateNode *> m_gate_map;
  std::vector<const GateNode *> m_gate_list;

  void calculate();
  void processPhi(PHINode *PN, Instruction *insertion_pt);
  FlowMap processIncomingValues(PHINode *PN, Instruction *insertion_pt,
                                bool &is_lowerable);

  GateKind classifyKind(PHINode *PN) const;
  const GateExpr *makeBottom(Type *Ty);
  const GateExpr *makeLeaf(Type *Ty, Value *V);
  const GateExpr *makeSelect(Type *Ty, GateGuard true_guard,
                             GateGuard false_guard,
                             const GateExpr *true_expr,
                             const GateExpr *false_expr);
  const GateExpr *makeSwitch(Type *Ty, std::vector<GateExpr::SwitchArm> arms);
  const GateExpr *makeBranchExpr(Type *Ty, GateGuard true_guard,
                                 GateGuard false_guard,
                                 const GateExpr *true_expr,
                                 const GateExpr *false_expr,
                                 bool allow_thinning);
  const GateExpr *makeExplicitSwitchExpr(
      Type *Ty, std::vector<GateExpr::SwitchArm> arms);
  const GateExpr *findFlowingExpr(BasicBlock *BB, FlowMap &flowing_values,
                                  Type *Ty) const;
  bool isBottomExpr(const GateExpr *Expr) const;
};

GateKind GateAnalysisImpl::classifyKind(PHINode *PN) const {
  if (m_LI.isLoopHeader(PN->getParent()))
    return GateKind::Mu;

  BasicBlock *BB = PN->getParent();
  for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
    BasicBlock *IncBB = PN->getIncomingBlock(I);
    Loop *IncLoop = m_LI.getLoopFor(IncBB);
    if (IncLoop && !IncLoop->contains(BB))
      return GateKind::Eta;
  }

  return GateKind::Gamma;
}

const GateExpr *GateAnalysisImpl::makeBottom(Type *Ty) {
  auto Expr = std::make_unique<GateExpr>(Ty);
  const GateExpr *Raw = Expr.get();
  m_owned_exprs.push_back(std::move(Expr));
  return Raw;
}

const GateExpr *GateAnalysisImpl::makeLeaf(Type *Ty, Value *V) {
  auto Expr = std::make_unique<GateExpr>(Ty, V);
  const GateExpr *Raw = Expr.get();
  m_owned_exprs.push_back(std::move(Expr));
  return Raw;
}

const GateExpr *GateAnalysisImpl::makeSelect(Type *Ty, GateGuard true_guard,
                                             GateGuard false_guard,
                                             const GateExpr *true_expr,
                                             const GateExpr *false_expr) {
  auto Expr = std::make_unique<GateExpr>(Ty, std::move(true_guard),
                                         std::move(false_guard), true_expr,
                                         false_expr);
  const GateExpr *Raw = Expr.get();
  m_owned_exprs.push_back(std::move(Expr));
  return Raw;
}

const GateExpr *
GateAnalysisImpl::makeSwitch(Type *Ty, std::vector<GateExpr::SwitchArm> arms) {
  auto Expr = std::make_unique<GateExpr>(Ty, std::move(arms));
  const GateExpr *Raw = Expr.get();
  m_owned_exprs.push_back(std::move(Expr));
  return Raw;
}

bool GateAnalysisImpl::isBottomExpr(const GateExpr *Expr) const {
  return Expr != nullptr && Expr->getKind() == GateExpr::Kind::Bottom;
}

const GateExpr *GateAnalysisImpl::makeBranchExpr(Type *Ty, GateGuard true_guard,
                                                 GateGuard false_guard,
                                                 const GateExpr *true_expr,
                                                 const GateExpr *false_expr,
                                                 bool allow_thinning) {
  if (isBottomExpr(true_expr) && isBottomExpr(false_expr))
    return makeBottom(Ty);

  if (true_expr == false_expr)
    return true_expr;

  if (allow_thinning && m_is_thinned) {
    if (isBottomExpr(true_expr))
      return false_expr;
    if (isBottomExpr(false_expr))
      return true_expr;
  }

  return makeSelect(Ty, std::move(true_guard), std::move(false_guard),
                    true_expr, false_expr);
}

const GateExpr *GateAnalysisImpl::makeExplicitSwitchExpr(
    Type *Ty, std::vector<GateExpr::SwitchArm> arms) {
  bool has_non_bottom = false;
  for (const auto &Arm : arms) {
    if (!isBottomExpr(Arm.expr)) {
      has_non_bottom = true;
      break;
    }
  }

  if (!has_non_bottom)
    return makeBottom(Ty);

  return makeSwitch(Ty, std::move(arms));
}

const GateExpr *GateAnalysisImpl::findFlowingExpr(BasicBlock *BB,
                                                  FlowMap &flowing_values,
                                                  Type *Ty) const {
  BasicBlock *PostDomBlock = BB;
  while (PostDomBlock) {
    auto It = flowing_values.find(PostDomBlock);
    if (It != flowing_values.end())
      return It->second;

    auto *PDTNode = m_PDT.getNode(PostDomBlock);
    if (!PDTNode)
      break;
    auto *IDom = PDTNode->getIDom();
    if (!IDom)
      break;
    PostDomBlock = IDom->getBlock();
  }

  (void)Ty;
  return nullptr;
}

void GateAnalysisImpl::calculate() {
  std::vector<PHINode *> Phis;
  DenseMap<BasicBlock *, Instruction *> insertion_points;

  for (auto &BB : m_function) {
    if (!m_DT.getNode(&BB))
      continue;

    Instruction *InsertionPoint = BB.getFirstNonPHI();
    assert(InsertionPoint && "Basic block has no non-PHI instruction");
    insertion_points[&BB] = InsertionPoint;

    for (auto &PN : BB.phis())
      Phis.push_back(&PN);
  }

  for (PHINode *PN : Phis) {
    auto *BB = PN->getParent();
    processPhi(PN, insertion_points.lookup(BB));
  }
}

GateAnalysisImpl::FlowMap
GateAnalysisImpl::processIncomingValues(PHINode *PN, Instruction *insertion_pt,
                                        bool &is_lowerable) {
  assert(PN && insertion_pt && "Incoming values require a PHI and insertion");

  FlowMap incoming_block_to_expr;
  BasicBlock *CurrentBB = PN->getParent();
  Type *PhiTy = PN->getType();

  for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
    BasicBlock *IncomingBlock = PN->getIncomingBlock(I);
    Value *IncomingValue = PN->getIncomingValue(I);

    const GateExpr *IncomingExpr =
        dominatesForUse(IncomingValue, insertion_pt, m_DT)
            ? makeLeaf(PhiTy, IncomingValue)
            : makeBottom(PhiTy);
    incoming_block_to_expr[IncomingBlock] = IncomingExpr;

    if (m_is_thinned)
      continue;

    if (!m_DT.dominates(IncomingBlock, CurrentBB))
      continue;

    auto *TI = IncomingBlock->getTerminator();
    if (!TI)
      continue;

    if (auto *BI = dyn_cast<BranchInst>(TI)) {
      if (BI->isUnconditional())
        continue;

      if (!dominatesForUse(BI->getCondition(), insertion_pt, m_DT))
        is_lowerable = false;

      GateGuard TrueGuard(GuardKind::BranchTrue, IncomingBlock,
                          BI->getSuccessor(0), BI->getCondition());
      GateGuard FalseGuard(GuardKind::BranchFalse, IncomingBlock,
                           BI->getSuccessor(1), BI->getCondition());

      const GateExpr *TrueExpr =
          BI->getSuccessor(0) == CurrentBB ? IncomingExpr : makeBottom(PhiTy);
      const GateExpr *FalseExpr =
          BI->getSuccessor(1) == CurrentBB ? IncomingExpr : makeBottom(PhiTy);

      incoming_block_to_expr[IncomingBlock] =
          makeBranchExpr(PhiTy, std::move(TrueGuard), std::move(FalseGuard),
                         TrueExpr, FalseExpr, false);
      continue;
    }

    if (auto *SI = dyn_cast<SwitchInst>(TI)) {
      if (!dominatesForUse(SI->getCondition(), insertion_pt, m_DT))
        is_lowerable = false;

      std::vector<GateExpr::SwitchArm> Arms;
      Arms.reserve(SI->getNumCases() + 1);

      for (auto Case : SI->cases()) {
        BasicBlock *Succ = Case.getCaseSuccessor();
        Arms.push_back(
            {GateGuard(GuardKind::SwitchCase, IncomingBlock, Succ,
                       SI->getCondition(), Case.getCaseValue()),
             Succ == CurrentBB ? IncomingExpr : makeBottom(PhiTy)});
      }

      BasicBlock *DefaultSucc = SI->getDefaultDest();
      Arms.push_back(
          {GateGuard(GuardKind::SwitchDefault, IncomingBlock, DefaultSucc,
                     SI->getCondition()),
           DefaultSucc == CurrentBB ? IncomingExpr : makeBottom(PhiTy)});

      incoming_block_to_expr[IncomingBlock] =
          makeExplicitSwitchExpr(PhiTy, std::move(Arms));
      continue;
    }

    if (auto *II = dyn_cast<InvokeInst>(TI)) {
      is_lowerable = false;

      std::vector<GateExpr::SwitchArm> Arms;
      Arms.reserve(2);
      Arms.push_back({GateGuard(GuardKind::InvokeNormal, IncomingBlock,
                                II->getNormalDest()),
                      II->getNormalDest() == CurrentBB ? IncomingExpr
                                                       : makeBottom(PhiTy)});
      Arms.push_back({GateGuard(GuardKind::InvokeUnwind, IncomingBlock,
                                II->getUnwindDest()),
                      II->getUnwindDest() == CurrentBB ? IncomingExpr
                                                       : makeBottom(PhiTy)});

      incoming_block_to_expr[IncomingBlock] =
          makeExplicitSwitchExpr(PhiTy, std::move(Arms));
      continue;
    }

    SmallVector<BasicBlock *, 4> Succs;
    for (BasicBlock *Succ : successors(IncomingBlock))
      Succs.push_back(Succ);
    if (Succs.size() <= 1)
      continue;

    is_lowerable = false;
    std::vector<GateExpr::SwitchArm> Arms;
    Arms.reserve(Succs.size());
    for (BasicBlock *Succ : Succs) {
      Arms.push_back({GateGuard(GuardKind::Opaque, IncomingBlock, Succ),
                      Succ == CurrentBB ? IncomingExpr : makeBottom(PhiTy)});
    }
    incoming_block_to_expr[IncomingBlock] =
        makeExplicitSwitchExpr(PhiTy, std::move(Arms));
  }

  return incoming_block_to_expr;
}

void GateAnalysisImpl::processPhi(PHINode *PN, Instruction *insertion_pt) {
  assert(PN && insertion_pt && "Gate construction requires a valid PHI");

  BasicBlock *CurrentBB = PN->getParent();
  Type *PhiTy = PN->getType();
  bool is_lowerable = true;
  FlowMap incoming_block_to_expr =
      processIncomingValues(PN, insertion_pt, is_lowerable);

  auto GreaterThanTopo = [this](BasicBlock *First, BasicBlock *Second) {
    assert(m_CDA.isTracked(*First) && m_CDA.isTracked(*Second) &&
           "Control-dependent blocks must be tracked");
    return m_CDA.getBBTopoIdx(First) > m_CDA.getBBTopoIdx(Second);
  };

  std::set<BasicBlock *, decltype(GreaterThanTopo)> cd_info(GreaterThanTopo);
  for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
    BasicBlock *IncomingBB = PN->getIncomingBlock(I);
    for (BasicBlock *CDBlock : m_CDA.getCDBlocks(IncomingBB))
      cd_info.insert(CDBlock);
  }

  FlowMap flowing_values;
  for (const auto &Entry : incoming_block_to_expr)
    flowing_values[Entry.first] = Entry.second;

  for (BasicBlock *BB : cd_info) {
    auto *TI = BB->getTerminator();
    if (!TI) {
      flowing_values[BB] = makeBottom(PhiTy);
      continue;
    }

    SmallDenseMap<BasicBlock *, const GateExpr *, 4> succ_to_expr;
    for (BasicBlock *Succ : successors(BB)) {
      const GateExpr *SuccExpr = nullptr;

      if (Succ == CurrentBB) {
        auto It = incoming_block_to_expr.find(BB);
        if (It != incoming_block_to_expr.end())
          SuccExpr = It->second;
      }

      if (SuccExpr == nullptr)
        SuccExpr = findFlowingExpr(Succ, flowing_values, PhiTy);
      if (SuccExpr == nullptr)
        SuccExpr = makeBottom(PhiTy);

      succ_to_expr[Succ] = SuccExpr;
    }

    if (auto *BI = dyn_cast<BranchInst>(TI)) {
      if (succ_to_expr.empty()) {
        flowing_values[BB] = makeBottom(PhiTy);
        continue;
      }

      if (BI->isUnconditional() || succ_to_expr.size() == 1) {
        flowing_values[BB] = succ_to_expr.begin()->second;
        continue;
      }

      if (!dominatesForUse(BI->getCondition(), insertion_pt, m_DT))
        is_lowerable = false;

      GateGuard TrueGuard(GuardKind::BranchTrue, BB, BI->getSuccessor(0),
                          BI->getCondition());
      GateGuard FalseGuard(GuardKind::BranchFalse, BB, BI->getSuccessor(1),
                           BI->getCondition());
      const GateExpr *TrueExpr = succ_to_expr.lookup(BI->getSuccessor(0));
      const GateExpr *FalseExpr = succ_to_expr.lookup(BI->getSuccessor(1));

      flowing_values[BB] =
          makeBranchExpr(PhiTy, std::move(TrueGuard), std::move(FalseGuard),
                         TrueExpr, FalseExpr, true);
      continue;
    }

    if (auto *SI = dyn_cast<SwitchInst>(TI)) {
      if (!dominatesForUse(SI->getCondition(), insertion_pt, m_DT))
        is_lowerable = false;

      std::vector<GateExpr::SwitchArm> Arms;
      Arms.reserve(SI->getNumCases() + 1);
      for (auto Case : SI->cases()) {
        BasicBlock *Succ = Case.getCaseSuccessor();
        Arms.push_back(
            {GateGuard(GuardKind::SwitchCase, BB, Succ, SI->getCondition(),
                       Case.getCaseValue()),
             succ_to_expr.lookup(Succ)});
      }

      BasicBlock *DefaultSucc = SI->getDefaultDest();
      Arms.push_back({GateGuard(GuardKind::SwitchDefault, BB, DefaultSucc,
                                SI->getCondition()),
                      succ_to_expr.lookup(DefaultSucc)});

      flowing_values[BB] = makeExplicitSwitchExpr(PhiTy, std::move(Arms));
      continue;
    }

    if (auto *II = dyn_cast<InvokeInst>(TI)) {
      is_lowerable = false;

      std::vector<GateExpr::SwitchArm> Arms;
      Arms.reserve(2);
      Arms.push_back({GateGuard(GuardKind::InvokeNormal, BB,
                                II->getNormalDest()),
                      succ_to_expr.lookup(II->getNormalDest())});
      Arms.push_back({GateGuard(GuardKind::InvokeUnwind, BB,
                                II->getUnwindDest()),
                      succ_to_expr.lookup(II->getUnwindDest())});

      flowing_values[BB] = makeExplicitSwitchExpr(PhiTy, std::move(Arms));
      continue;
    }

    SmallVector<BasicBlock *, 4> Succs;
    for (BasicBlock *Succ : successors(BB))
      Succs.push_back(Succ);
    if (Succs.empty()) {
      flowing_values[BB] = makeBottom(PhiTy);
      continue;
    }

    if (Succs.size() == 1) {
      flowing_values[BB] = succ_to_expr.lookup(Succs.front());
      continue;
    }

    is_lowerable = false;
    std::vector<GateExpr::SwitchArm> Arms;
    Arms.reserve(Succs.size());
    for (BasicBlock *Succ : Succs) {
      Arms.push_back(
          {GateGuard(GuardKind::Opaque, BB, Succ), succ_to_expr.lookup(Succ)});
    }
    flowing_values[BB] = makeExplicitSwitchExpr(PhiTy, std::move(Arms));
  }

  auto *DomNode = m_DT.getNode(CurrentBB);
  assert(DomNode && DomNode->getIDom() && "PHI in entry block is unexpected");
  BasicBlock *IDomBlock = DomNode->getIDom()->getBlock();
  const GateExpr *RootExpr =
      flowing_values.count(IDomBlock) ? flowing_values.lookup(IDomBlock)
                                      : makeBottom(PhiTy);

  auto Node = std::make_unique<GateNode>(PN, classifyKind(PN), PhiTy, RootExpr,
                                         is_lowerable);
  const GateNode *RawNode = Node.get();
  m_owned_gates.push_back(std::move(Node));
  m_gate_map[PN] = RawNode;
  m_gate_list.push_back(RawNode);
}

} // namespace

char GateAnalysisPass::ID = 0;

GateAnalysisPass::GateAnalysisPass() : ModulePass(ID), m_thinned(ThinnedGsa) {}

GateAnalysisPass::GateAnalysisPass(bool thinned)
    : ModulePass(ID), m_thinned(thinned) {}

void GateAnalysisPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ControlDependenceAnalysisPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<PostDominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

bool GateAnalysisPass::runOnModule(Module &M) {
  auto &CDP = getAnalysis<ControlDependenceAnalysisPass>();

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    auto &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    runOnFunction(F, CDP.getControlDependenceAnalysis(F), LI);
  }

  return false;
}

bool GateAnalysisPass::runOnFunction(Function &F, ControlDependenceAnalysis &CDA,
                                     LoopInfo &LI) {
  auto &DT = getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
  auto &PDT = getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();

  m_analyses[&F] =
      std::make_unique<GateAnalysisImpl>(F, DT, PDT, LI, CDA, m_thinned);
  return false;
}

StringRef GateAnalysisPass::getPassName() const { return "GateAnalysisPass"; }

void GateAnalysisPass::print(raw_ostream &os, const Module *) const {
  os << "GateAnalysisPass::print\n";
}

bool GateAnalysisPass::hasAnalysisFor(const Function &F) const {
  return m_analyses.count(&F) > 0;
}

GateAnalysis &GateAnalysisPass::getGateAnalysis(const Function &F) {
  assert(hasAnalysisFor(F));
  return *m_analyses[&F];
}

ModulePass *createGateAnalysisPass() { return new GateAnalysisPass(); }

} // namespace gsa

static RegisterPass<gsa::GateAnalysisPass>
    GsaGA("gsa-gated-ssa", "Compute read-only Gated SSA form", true, true);

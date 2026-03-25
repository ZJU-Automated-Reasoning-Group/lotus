//===- GsaMaterialization.cpp - Materialize GSA expressions ---------------===//
//
// Lowers immutable GSA expressions into LLVM IR by creating select chains and
// optionally replacing the source PHI nodes.
//
//===----------------------------------------------------------------------===//

#include "IR/GSA/GSA.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

namespace gsa {

static cl::opt<bool>
    GsaReplacePhis("gsa-replace-phis",
                   cl::desc("Replace PHI nodes with materialized GSA nodes"),
                   cl::init(true), cl::Hidden);

namespace {

static StringRef gateKindName(GateKind kind) {
  switch (kind) {
  case GateKind::Gamma:
    return "gamma";
  case GateKind::Mu:
    return "mu";
  case GateKind::Eta:
    return "eta";
  }
  llvm_unreachable("Unknown gate kind");
}

class GsaMaterializer {
public:
  GsaMaterializer(Function &F, GateAnalysis &GA, bool replace_phis)
      : m_function(F), m_gate_analysis(GA), m_IRB(F.getContext()),
        m_replace_phis(replace_phis) {}

  bool run();

private:
  Function &m_function;
  GateAnalysis &m_gate_analysis;
  IRBuilder<> m_IRB;
  bool m_replace_phis{true};
  bool m_changed{false};

  Value *lowerExpr(const GateExpr *Expr, Instruction *insertion_pt,
                   DenseMap<const GateExpr *, Value *> &cache,
                   GateKind gate_kind);
};

Value *GsaMaterializer::lowerExpr(const GateExpr *Expr, Instruction *insertion_pt,
                                  DenseMap<const GateExpr *, Value *> &cache,
                                  GateKind gate_kind) {
  auto It = cache.find(Expr);
  if (It != cache.end())
    return It->second;

  Value *Result = nullptr;
  switch (Expr->getKind()) {
  case GateExpr::Kind::Bottom:
    Result = PoisonValue::get(Expr->getType());
    break;

  case GateExpr::Kind::LeafValue:
    Result = Expr->getLeafValue();
    break;

  case GateExpr::Kind::Select: {
    Value *TrueValue =
        lowerExpr(Expr->getTrueExpr(), insertion_pt, cache, gate_kind);
    Value *FalseValue =
        lowerExpr(Expr->getFalseExpr(), insertion_pt, cache, gate_kind);
    Value *Cond = Expr->getTrueGuard().getCondition();
    assert(Cond && "Lowerable select expressions must carry a condition");

    m_IRB.SetInsertPoint(insertion_pt);
    Result = m_IRB.CreateSelect(
        Cond, TrueValue, FalseValue,
        Twine("lotus.gsa.") + gateKindName(gate_kind) + "." +
            Expr->getTrueGuard().getControlBlock()->getName());
    m_changed = true;
    break;
  }

  case GateExpr::Kind::Switch: {
    Value *Accum = PoisonValue::get(Expr->getType());
    Value *MatchedCase = nullptr;
    const GateExpr *DefaultExpr = nullptr;
    Value *SwitchCond = nullptr;

    for (const auto &Arm : Expr->getSwitchArms()) {
      if (Arm.guard.getKind() == GuardKind::SwitchDefault) {
        DefaultExpr = Arm.expr;
        if (!SwitchCond)
          SwitchCond = Arm.guard.getCondition();
        continue;
      }

      assert(Arm.guard.getKind() == GuardKind::SwitchCase &&
             "Only switch case/default arms are lowerable");
      if (!SwitchCond)
        SwitchCond = Arm.guard.getCondition();

      Value *ArmValue = lowerExpr(Arm.expr, insertion_pt, cache, gate_kind);
      m_IRB.SetInsertPoint(insertion_pt);
      Value *Cmp = m_IRB.CreateICmpEQ(
          SwitchCond, Arm.guard.getCaseValue(),
          Twine("lotus.gsa.case.") + Arm.guard.getControlBlock()->getName());
      if (!MatchedCase)
        MatchedCase = Cmp;
      else
        MatchedCase =
            m_IRB.CreateOr(MatchedCase, Cmp,
                           Twine("lotus.gsa.case.any.") +
                               Arm.guard.getControlBlock()->getName());

      Result = m_IRB.CreateSelect(
          Cmp, ArmValue, Accum,
          Twine("lotus.gsa.") + gateKindName(gate_kind) + ".switch." +
              Arm.guard.getControlBlock()->getName());
      Accum = Result;
      m_changed = true;
    }

    if (DefaultExpr) {
      Value *DefaultValue =
          lowerExpr(DefaultExpr, insertion_pt, cache, gate_kind);
      m_IRB.SetInsertPoint(insertion_pt);
      Value *DefaultTaken =
          MatchedCase ? m_IRB.CreateNot(MatchedCase,
                                        Twine("lotus.gsa.default.") +
                                            gateKindName(gate_kind))
                      : m_IRB.getTrue();
      Result = m_IRB.CreateSelect(
          DefaultTaken, DefaultValue, Accum,
          Twine("lotus.gsa.") + gateKindName(gate_kind) + ".default");
      Accum = Result;
      m_changed = true;
    }

    Result = Accum;
    break;
  }
  }

  cache[Expr] = Result;
  return Result;
}

bool GsaMaterializer::run() {
  DenseMap<BasicBlock *, Instruction *> insertion_points;
  for (const GateNode *Gate : m_gate_analysis.gates()) {
    PHINode *PN = Gate->getSourcePhi();
    if (!PN)
      continue;

    BasicBlock *BB = PN->getParent();
    if (insertion_points.count(BB) == 0) {
      Instruction *InsertionPoint = BB->getFirstNonPHI();
      assert(InsertionPoint && "Basic block has no non-PHI instruction");
      insertion_points[BB] = InsertionPoint;
    }
  }

  for (const GateNode *Gate : m_gate_analysis.gates()) {
    PHINode *PN = Gate->getSourcePhi();
    if (!PN || !Gate->isLowerable())
      continue;

    DenseMap<const GateExpr *, Value *> cache;
    Value *Materialized =
        lowerExpr(Gate->getRootExpr(), insertion_points.lookup(PN->getParent()),
                  cache, Gate->getKind());
    if (!m_replace_phis)
      continue;

    PN->replaceAllUsesWith(Materialized);
    PN->eraseFromParent();
    m_changed = true;
  }

  return m_changed;
}

} // namespace

char GsaMaterializationPass::ID = 0;

GsaMaterializationPass::GsaMaterializationPass()
    : ModulePass(ID), m_replace_phis(GsaReplacePhis) {}

GsaMaterializationPass::GsaMaterializationPass(bool replace_phis)
    : ModulePass(ID), m_replace_phis(replace_phis) {}

void GsaMaterializationPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GateAnalysisPass>();
  AU.setPreservesCFG();
}

bool GsaMaterializationPass::runOnModule(Module &M) {
  auto &GA = getAnalysis<GateAnalysisPass>();
  bool Changed = false;
  for (auto &F : M) {
    if (F.isDeclaration() || !GA.hasAnalysisFor(F))
      continue;
    Changed |= runOnFunction(F, GA.getGateAnalysis(F));
  }
  return Changed;
}

bool GsaMaterializationPass::runOnFunction(Function &F, GateAnalysis &GA) {
  GsaMaterializer Materializer(F, GA, m_replace_phis);
  return Materializer.run();
}

StringRef GsaMaterializationPass::getPassName() const {
  return "GsaMaterializationPass";
}

void GsaMaterializationPass::print(raw_ostream &os, const Module *) const {
  os << "GsaMaterializationPass::print\n";
}

ModulePass *createGsaMaterializationPass() {
  return new GsaMaterializationPass();
}

} // namespace gsa

static RegisterPass<gsa::GsaMaterializationPass>
    GsaMaterializerPass("gsa-materialize",
                        "Materialize Gated SSA expressions", false, false);

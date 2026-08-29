/*
 * Available Expressions Analysis (Forward, must-analysis)
 *
 * An expression e is "available" at program point p if every path from the
 * function entry to p evaluates e and does not subsequently redefine any
 * operand of e.  This is a forward, intersection-based (must) analysis.
 *
 * Author: rainoftime
 */
#include "Dataflow/Mono/Analyses/Intra/AvailableExpressions.h"

#include "llvm/IR/Instructions.h"

#include "Dataflow/Mono/Domains/AvailableExpressionsDomain.h"
#include "Dataflow/Mono/LLVM/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

#include <unordered_set>

using namespace llvm;

namespace mono {

namespace {

/**
 * @brief Extract expressions computed by an instruction
 *
 * Currently handles binary operators, casts, and comparisons.
 */
static std::vector<AvailableExpression> getComputedExpressions(Instruction *Inst) {
  std::vector<AvailableExpression> Exprs;

  if (auto *BinOp = dyn_cast<BinaryOperator>(Inst)) {
    SmallVector<Value *, 2> Ops{BinOp->getOperand(0), BinOp->getOperand(1)};
    Exprs.emplace_back(BinOp->getOpcode(), Ops);
  } else if (auto *Cast = dyn_cast<CastInst>(Inst)) {
    SmallVector<Value *, 1> Ops{Cast->getOperand(0)};
    Exprs.emplace_back(Cast->getOpcode(), Ops);
  } else if (auto *Cmp = dyn_cast<CmpInst>(Inst)) {
    SmallVector<Value *, 2> Ops{Cmp->getOperand(0), Cmp->getOperand(1)};
    Exprs.emplace_back(Cmp->getOpcode(), Ops);
  }
  // Could extend to handle GEPs, calls to pure functions, etc.

  return Exprs;
}

/**
 * @brief Find expressions that are killed (invalidated) by an instruction
 *
 * An expression is killed if the instruction redefines one of its operands.
 * In SSA form, this is straightforward: we kill all expressions that use
 * the value being defined.
 */
static std::set<AvailableExpression>
getKilledExpressions(Instruction *Inst, const std::set<AvailableExpression> &AllExprs) {
  std::set<AvailableExpression> Killed;

  // In SSA form, we kill expressions that use the value being redefined
  if (!Inst->getType()->isVoidTy()) {
    for (const auto &Expr : AllExprs) {
      if (Expr.usesValue(Inst)) {
        Killed.insert(Expr);
      }
    }
  }

  return Killed;
}

// ============================================================================
// Available Expressions Problem (Forward, must-analysis)
// ============================================================================

class AvailableExprProblem : public IntraMonoProblem<AvailableExpressionsDomain> {
public:
  explicit AvailableExprProblem(Function *F)
      : IntraMonoProblem<AvailableExpressionsDomain>({F}) {
    // Collect all expressions in the function for use as allTop().
    for (auto &BB : *F) {
      for (auto &Inst : BB) {
        auto Exprs = getComputedExpressions(&Inst);
        AllExpressions.insert(Exprs.begin(), Exprs.end());
      }
    }
  }

  // Available Expressions is a FORWARD analysis: facts flow from entry to exit.
  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Forward;
  }

  // Transfer function: OUT[n] = GEN[n] ∪ (IN[n] − KILL[n])
  std::set<AvailableExpression> normalFlow(Instruction *Inst,
                                  const std::set<AvailableExpression> &In) override {
    std::set<AvailableExpression> Out = In;

    // KILL: remove expressions that use the value defined by this instruction.
    // In SSA form every value is defined exactly once, so KILL is non-empty
    // only for instructions that produce a value used in some expression.
    auto Killed = getKilledExpressions(Inst, Out);
    for (const auto &Expr : Killed) {
      Out.erase(Expr);
    }

    // GEN: add expressions computed by this instruction (after killing).
    auto Generated = getComputedExpressions(Inst);
    Out.insert(Generated.begin(), Generated.end());

    return Out;
  }

  // Merge = intersection: an expression is available only if available on
  // ALL paths reaching this point (must-analysis).
  std::set<AvailableExpression> merge(const std::set<AvailableExpression> &Lhs,
                             const std::set<AvailableExpression> &Rhs) override {
    std::set<AvailableExpression> Out;
    std::set_intersection(Lhs.begin(), Lhs.end(), Rhs.begin(), Rhs.end(),
                          std::inserter(Out, Out.begin()));
    return Out;
  }

  bool equal_to(const std::set<AvailableExpression> &Lhs,
                const std::set<AvailableExpression> &Rhs) override {
    return Lhs == Rhs;
  }

  // allTop() = universal set (all expressions).
  // For a must-analysis with intersection as merge, the top of the lattice
  // is the set of ALL expressions (every expression is "assumed available"
  // until proven otherwise).  This is the correct initial value for nodes
  // that have not yet been reached.
  std::set<AvailableExpression> allTop() override { return AllExpressions; }

  std::unordered_map<Instruction *, std::set<AvailableExpression>>
  initialSeeds() override {
    std::unordered_map<Instruction *, std::set<AvailableExpression>> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }

    // For a forward analysis, seed the function entry instruction with the
    // empty set: no expressions are available before the first instruction.
    Seeds[&F->getEntryBlock().front()] = {};
    return Seeds;
  }

private:
  std::set<AvailableExpression> AllExpressions;
};

// Build a map from AvailableExpression → the Instruction* that computes it.
// When multiple instructions compute the same expression (same opcode +
// operands), we keep the first one encountered in program order.
static std::map<AvailableExpression, Instruction *> buildExprToInstMap(Function *F) {
  std::map<AvailableExpression, Instruction *> Map;
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      for (const auto &Expr : getComputedExpressions(&Inst)) {
        Map.emplace(Expr, &Inst); // emplace keeps the first insertion
      }
    }
  }
  return Map;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

std::unique_ptr<DataFlowResult> runAvailableExpressionsAnalysis(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return nullptr;
  }

  AvailableExprProblem Problem(F);
  IntraMonoSolver<AvailableExpressionsDomain> Solver(Problem);
  Solver.solve();

  // build a correct result by mapping each AvailableExpression back to the
  // Instruction* that computes it.  DataFlowResult stores std::set<Value*>;
  // we represent each available expression by the instruction that defines it.
  //
  // This is a FORWARD analysis, so the solver's IN/OUT maps directly to the
  // analysis IN/OUT without any swap:
  //   Solver IN[i]  = IN[i]  = expressions available BEFORE i executes
  //   Solver OUT[i] = OUT[i] = expressions available AFTER  i executes
  //                          = normalFlow(i, IN[i])
  //                          = GEN[i] ∪ (IN[i] − KILL[i])

  const auto ExprToInst = buildExprToInstMap(F);

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      auto *I = &Inst;

      // Forward analysis: SolverIn = IN[I], SolverOut = OUT[I].
      const auto &SolverIn = Solver.getInResultsAt(I);   // std::set<AvailableExpression>
      const auto &SolverOut = Solver.getOutResultsAt(I); // std::set<AvailableExpression>

      // Populate Result->IN(I): expressions available before I.
      for (const auto &Expr : SolverIn) {
        auto It = ExprToInst.find(Expr);
        if (It != ExprToInst.end()) {
          Result->IN(I).insert(It->second);
        }
      }

      // Populate Result->OUT(I): expressions available after I.
      for (const auto &Expr : SolverOut) {
        auto It = ExprToInst.find(Expr);
        if (It != ExprToInst.end()) {
          Result->OUT(I).insert(It->second);
        }
      }

      // GEN[I]: expressions computed by I.
      for (const auto &Expr : getComputedExpressions(I)) {
        (void)Expr;
        Result->GEN(I).insert(I);
      }

      // KILL[I]: expressions whose operands are redefined by I.
      // In SSA form this is always empty (each value defined exactly once),
      // but we compute it correctly for completeness / non-SSA IR.
      for (const auto &Expr : getKilledExpressions(I, SolverIn)) {
        auto It = ExprToInst.find(Expr);
        if (It != ExprToInst.end()) {
          Result->KILL(I).insert(It->second);
        }
      }
    }
  }

  return Result;
}

} // namespace mono

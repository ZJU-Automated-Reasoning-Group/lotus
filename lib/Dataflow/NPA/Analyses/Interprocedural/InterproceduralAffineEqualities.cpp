/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralAffineEqualities.h"

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"

#include <algorithm>
#include <unordered_set>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

namespace {

using D = AffineRelationDomain;
using Exp = Exp0<D>;
using E = E0<D>;
using Relation = D::value_type;
using Component = AffineRelationComponent;
using Row = std::vector<llvm::APInt>;

bool isTrackedScalar(const llvm::Value *V) {
  auto *Ty = V ? V->getType() : nullptr;
  return Ty && Ty->isIntegerTy() && Ty->getIntegerBitWidth() <= 64;
}

bool getConstantIntValue(const llvm::Value *V, int64_t &out) {
  auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(V);
  if (!CI || CI->getBitWidth() > 64)
    return false;
  out = CI->getBitWidth() == 1 ? static_cast<int64_t>(CI->getZExtValue())
                               : CI->getSExtValue();
  return true;
}

int64_t wrapToBitWidth(int64_t value, unsigned bitWidth) {
  llvm::APInt wrapped(bitWidth, static_cast<uint64_t>(value), true);
  return bitWidth == 1 ? static_cast<int64_t>(wrapped.getZExtValue())
                       : wrapped.getSExtValue();
}

AffineExpr topExpr() { return {}; }

AffineExpr constExpr(int64_t value) {
  AffineExpr out;
  out.top = false;
  out.constant = value;
  return out;
}

AffineExpr normalizeExpr(AffineExpr expr, unsigned bitWidth) {
  if (expr.top)
    return expr;
  expr.constant = wrapToBitWidth(expr.constant, bitWidth);
  for (auto It = expr.terms.begin(); It != expr.terms.end();) {
    It->second = wrapToBitWidth(It->second, bitWidth);
    if (It->second == 0)
      It = expr.terms.erase(It);
    else
      ++It;
  }
  return expr;
}

AffineExpr addExpr(AffineExpr lhs, const AffineExpr &rhs, unsigned bitWidth) {
  if (lhs.top || rhs.top)
    return topExpr();
  lhs.constant += rhs.constant;
  for (const auto &term : rhs.terms)
    lhs.terms[term.first] += term.second;
  return normalizeExpr(std::move(lhs), bitWidth);
}

AffineExpr scaleExpr(AffineExpr expr, int64_t factor, unsigned bitWidth) {
  if (expr.top)
    return topExpr();
  expr.constant *= factor;
  for (auto &term : expr.terms)
    term.second *= factor;
  return normalizeExpr(std::move(expr), bitWidth);
}

bool isZeroRow(const Row &row) {
  return std::all_of(row.begin(), row.end(),
                     [](const llvm::APInt &entry) { return entry.isZero(); });
}

int leadingIndex(const Row &row) {
  for (size_t i = 0; i < row.size(); ++i) {
    if (!row[i].isZero())
      return static_cast<int>(i);
  }
  return -1;
}

unsigned rankOf(const llvm::APInt &value) {
  return value.isZero() ? value.getBitWidth() : value.countTrailingZeros();
}

llvm::APInt oddInverse(const llvm::APInt &odd) {
  unsigned bitWidth = odd.getBitWidth();
  llvm::APInt inv(bitWidth, 1);
  llvm::APInt two(bitWidth, 2);
  for (unsigned bits = 1; bits < bitWidth; bits <<= 1)
    inv *= (two - odd * inv);
  return inv;
}

void scaleRow(Row &row, const llvm::APInt &factor) {
  for (auto &entry : row)
    entry *= factor;
}

void subtractScaledRow(Row &row, const Row &pivot, const llvm::APInt &factor) {
  for (size_t i = 0; i < row.size(); ++i)
    row[i] -= factor * pivot[i];
}

std::vector<Row> howellize(std::vector<Row> rows) {
  if (rows.empty())
    return rows;
  const unsigned bitWidth = rows.front().front().getBitWidth();
  const size_t cols = rows.front().size();
  size_t nextRow = 0;

  for (size_t col = 0; col < cols; ++col) {
    std::vector<size_t> candidates;
    for (size_t r = nextRow; r < rows.size(); ++r) {
      if (leadingIndex(rows[r]) == static_cast<int>(col))
        candidates.push_back(r);
    }
    if (candidates.empty())
      continue;

    size_t pivotPos = candidates.front();
    for (size_t idx : candidates) {
      if (rankOf(rows[idx][col]) < rankOf(rows[pivotPos][col]))
        pivotPos = idx;
    }

    unsigned pivotRank = rankOf(rows[pivotPos][col]);
    llvm::APInt oddPart = rows[pivotPos][col].lshr(pivotRank);
    scaleRow(rows[pivotPos], oddInverse(oddPart));

    for (size_t idx : candidates) {
      if (idx == pivotPos)
        continue;
      unsigned curRank = rankOf(rows[idx][col]);
      llvm::APInt factor(bitWidth, 1);
      factor <<= (curRank - pivotRank);
      factor *= rows[idx][col].lshr(curRank);
      subtractScaledRow(rows[idx], rows[pivotPos], factor);
    }

    rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
    auto it =
        std::find_if(rows.begin() + nextRow, rows.end(), [col](const Row &row) {
          return leadingIndex(row) == static_cast<int>(col);
        });
    if (it == rows.end())
      continue;
    std::iter_swap(rows.begin() + nextRow, it);
    const Row pivot = rows[nextRow];

    for (size_t upper = 0; upper < nextRow; ++upper) {
      llvm::APInt factor = rows[upper][col].lshr(pivotRank);
      subtractScaledRow(rows[upper], pivot, factor);
    }

    ++nextRow;
  }

  rows.erase(std::remove_if(rows.begin(), rows.end(), isZeroRow), rows.end());
  return rows;
}

Row reorderToPostPreConst(const Row &row, unsigned vars) {
  Row out(row.front().getBitWidth(), llvm::APInt(row.front().getBitWidth(), 0));
  out.clear();
  out.reserve(2 * vars + 1);
  out.insert(out.end(), row.begin() + vars, row.begin() + 2 * vars);
  out.insert(out.end(), row.begin(), row.begin() + vars);
  out.push_back(row.back());
  return out;
}

AffineState materializeAffineExpressionsImpl(const Relation &state) {
  AffineState out;
  out.reachable = !state.bottom;
  if (state.bottom)
    return out;

  const auto *vocab = D::getVocabulary();
  if (!vocab)
    return out;
  auto it = state.components.find(D::componentBitWidth());
  if (it == state.components.end())
    return out;

  const unsigned vars = static_cast<unsigned>(vocab->values.size());
  std::vector<Row> rows;
  rows.reserve(it->second.constraints.size());
  for (const Row &row : it->second.constraints)
    rows.push_back(reorderToPostPreConst(row, vars));
  rows = howellize(std::move(rows));

  std::vector<AffineExpr> solved(vars, topExpr());
  std::vector<AffineExpr> preSolved(vars, topExpr());

  bool progress = true;
  while (progress) {
    progress = false;

    for (unsigned preCol = 0; preCol < vars; ++preCol) {
      if (!preSolved[preCol].top)
        continue;
      for (const Row &row : rows) {
        if (row[vars + preCol].isZero())
          continue;
        bool hasPost = false;
        for (unsigned postCol = 0; postCol < vars; ++postCol) {
          if (!row[postCol].isZero()) {
            hasPost = true;
            break;
          }
        }
        if (hasPost)
          continue;

        llvm::APInt coeff = row[vars + preCol];
        if (!coeff[0])
          continue;
        llvm::APInt inv = oddInverse(coeff);
        unsigned actualWidth = vocab->actualBitWidths.at(vocab->values[preCol]);
        AffineExpr expr = constExpr((-row.back() * inv).getSExtValue());
        bool ok = true;
        for (unsigned other = 0; other < vars; ++other) {
          if (other == preCol || row[vars + other].isZero())
            continue;
          if (preSolved[other].top) {
            ok = false;
            break;
          }
          expr = addExpr(std::move(expr),
                         scaleExpr(preSolved[other],
                                   -(row[vars + other] * inv).getSExtValue(),
                                   actualWidth),
                         actualWidth);
        }
        if (ok) {
          preSolved[preCol] = normalizeExpr(std::move(expr), actualWidth);
          progress = true;
          break;
        }
      }
    }

    for (unsigned col = 0; col < vars; ++col) {
      if (!solved[col].top)
        continue;
      for (const Row &row : rows) {
        if (row[col].isZero())
          continue;
        llvm::APInt coeff = row[col];
        if (!coeff[0])
          continue;
        llvm::APInt inv = oddInverse(coeff);
        unsigned actualWidth = vocab->actualBitWidths.at(vocab->values[col]);
        AffineExpr expr = constExpr((-row.back() * inv).getSExtValue());
        bool ok = true;

        for (unsigned postCol = 0; postCol < vars; ++postCol) {
          if (postCol == col || row[postCol].isZero())
            continue;
          if (solved[postCol].top) {
            ok = false;
            break;
          }
          expr = addExpr(std::move(expr),
                         scaleExpr(solved[postCol],
                                   -(row[postCol] * inv).getSExtValue(),
                                   actualWidth),
                         actualWidth);
        }
        if (!ok)
          continue;

        for (unsigned preCol = 0; preCol < vars; ++preCol) {
          if (row[vars + preCol].isZero())
            continue;
          int64_t coeffInt = -(row[vars + preCol] * inv).getSExtValue();
          if (!preSolved[preCol].top) {
            expr = addExpr(std::move(expr),
                           scaleExpr(preSolved[preCol], coeffInt, actualWidth),
                           actualWidth);
          } else {
            expr.terms[vocab->values[preCol]] += coeffInt;
          }
        }
        solved[col] = normalizeExpr(std::move(expr), actualWidth);
        progress = true;
        break;
      }
    }
  }

  for (unsigned i = 0; i < vars; ++i) {
    if (!solved[i].top)
      out.values[vocab->values[i]] = solved[i];
    else if (!preSolved[i].top)
      out.values[vocab->values[i]] = preSolved[i];
  }
  return out;
}

class AffineRelationAnalysis {
public:
  using FactType = Relation;
  using Engine = InterproceduralEngine<D, AffineRelationAnalysis>;

  explicit AffineRelationAnalysis(llvm::Module &M) {
    buildVocabulary(M);
    D::configure(&Vocabulary);
  }

  FactType getEntryValue() const { return D::identity(); }

  Relation getEdgeTransfer(const llvm::Instruction &term,
                           const llvm::BasicBlock &succ) const {
    if (auto *Branch = llvm::dyn_cast<llvm::BranchInst>(&term)) {
      if (!Branch->isConditional())
        return D::identity();
      if (!D::isTrackedValue(Branch->getCondition()))
        return D::identity();
      return D::addPrecondition(D::identity(), Branch->getCondition(),
                                Branch->getSuccessor(0) == &succ ? 1 : 0);
    }
    if (auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(&term)) {
      if (!D::isTrackedValue(Switch->getCondition()))
        return D::identity();
      for (const auto &Case : Switch->cases()) {
        if (Case.getCaseSuccessor() == &succ) {
          return D::addPrecondition(
              D::identity(), Switch->getCondition(),
              static_cast<int64_t>(Case.getCaseValue()->getSExtValue()));
        }
      }
      return D::identity();
    }
    return D::identity();
  }

  E buildBlockEntryExpr(llvm::BasicBlock &BB, E inExpr) {
    auto *FirstPhi = llvm::dyn_cast<llvm::PHINode>(BB.begin());
    if (!FirstPhi)
      return inExpr;

    E result = nullptr;
    for (auto *Pred : predecessors(&BB)) {
      Relation phiTransfer = D::identity();
      for (auto &Inst : BB) {
        auto *Phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!Phi)
          break;
        if (!isTrackedScalar(Phi))
          continue;
        Relation assign =
            relationForValue(Phi, Phi->getIncomingValueForBlock(Pred));
        phiTransfer = D::extend(assign, phiTransfer);
      }
      E branch = Exp::hole(Engine::getBlockSymbol(Pred));
      if (auto *PredTerm = Pred->getTerminator()) {
        branch = Exp::seq(getEdgeTransfer(*PredTerm, BB), branch);
      }
      branch = Exp::seq(phiTransfer, branch);
      result = result ? Exp::ndet(result, branch) : branch;
    }
    return result ? result : inExpr;
  }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::PHINode>(&I) || llvm::isa<llvm::CallBase>(&I) ||
        I.getType()->isVoidTy())
      return currentPath;
    if (!isTrackedScalar(&I))
      return currentPath;
    return Exp::seq(buildInstructionRelation(I), currentPath);
  }

  Relation getCallEntryTransfer(const llvm::CallBase &Call,
                                const llvm::Function &Callee) {
    Relation relation = D::identity();
    const auto *ParamIt = Callee.arg_begin();
    for (unsigned i = 0; i < Call.arg_size() && ParamIt != Callee.arg_end();
         ++i, ++ParamIt) {
      if (!isTrackedScalar(&*ParamIt))
        continue;
      relation = D::extend(relationForValue(&*ParamIt, Call.getArgOperand(i)),
                           relation);
    }
    return relation;
  }

  Relation getCallReturnTransfer(const llvm::CallBase &Call,
                                 const llvm::Function &Callee) {
    if (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
      return D::identity();

    std::vector<Relation> branches;
    for (const auto &BB : Callee) {
      auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
      if (!Ret || !Ret->getReturnValue())
        continue;
      branches.push_back(relationForValue(&Call, Ret->getReturnValue()));
    }
    if (branches.empty())
      return D::makeForget(&Call);

    Relation out = branches.front();
    for (size_t i = 1; i < branches.size(); ++i)
      out = D::combine(out, branches[i]);
    return out;
  }

  Relation getCallToReturnTransfer(const llvm::CallBase &Call) {
    return (Call.getType()->isVoidTy() || !isTrackedScalar(&Call))
               ? D::identity()
               : D::makeForget(&Call);
  }

  FactType applySummary(const Relation &summary, const FactType &fact) {
    return D::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return D::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return D::equal(lhs, rhs);
  }

  InterproceduralAffineEqualities::Result
  buildResult(const typename Engine::Result &engineResult) const {
    InterproceduralAffineEqualities::Result result;
    result.status = engineResult.status;
    result.summaries.insert(engineResult.summaries.begin(),
                            engineResult.summaries.end());
    for (const auto &entry : engineResult.blockEntryFacts) {
      result.blockRelations.emplace(entry.first, entry.second);
    }
    return result;
  }

private:
  AffineRelationVocabulary Vocabulary;

  void buildVocabulary(llvm::Module &M) {
    std::set<const llvm::Value *> ordered;
    for (const auto &F : M) {
      if (F.isDeclaration())
        continue;
      for (const auto &Arg : F.args()) {
        if (isTrackedScalar(&Arg))
          ordered.insert(&Arg);
      }
      for (const auto &BB : F) {
        for (const auto &I : BB) {
          if (isTrackedScalar(&I))
            ordered.insert(&I);
        }
      }
    }
    Vocabulary.values.assign(ordered.begin(), ordered.end());
    for (unsigned i = 0; i < Vocabulary.values.size(); ++i) {
      Vocabulary.indices[Vocabulary.values[i]] = i;
      Vocabulary.actualBitWidths[Vocabulary.values[i]] =
          Vocabulary.values[i]->getType()->getIntegerBitWidth();
    }
  }

  Relation relationForValue(const llvm::Value *dest,
                            const llvm::Value *src) const {
    int64_t constant = 0;
    if (getConstantIntValue(src, constant))
      return D::makeAffineAssignment(
          dest, wrapToBitWidth(constant, D::bitWidthOf(dest)), {});
    if (D::isTrackedValue(src))
      return D::makeAffineAssignment(dest, 0, {{src, 1}});
    return D::makeForget(dest);
  }

  Relation buildInstructionRelation(llvm::Instruction &I) const {
    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I))
      return buildCastRelation(*Cast);
    if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I))
      return buildCompareRelation(*Cmp);
    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I))
      return buildSelectRelation(*Select);
    if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I))
      return buildBinaryRelation(*BinOp);
    return D::makeForget(&I);
  }

  Relation buildCastRelation(const llvm::CastInst &Cast) const {
    int64_t constant = 0;
    if (getConstantIntValue(Cast.getOperand(0), constant)) {
      switch (Cast.getOpcode()) {
      case llvm::Instruction::SExt:
      case llvm::Instruction::ZExt:
      case llvm::Instruction::Trunc:
        return D::makeAffineAssignment(
            &Cast, wrapToBitWidth(constant, D::bitWidthOf(&Cast)), {});
      default:
        return D::makeForget(&Cast);
      }
    }
    if (!D::isTrackedValue(Cast.getOperand(0)))
      return D::makeForget(&Cast);
    switch (Cast.getOpcode()) {
    case llvm::Instruction::SExt:
      return D::makeAffineAssignment(&Cast, 0, {{Cast.getOperand(0), 1}});
    case llvm::Instruction::ZExt:
      return D::bitWidthOf(Cast.getOperand(0)) == 1
                 ? D::makeAffineAssignment(&Cast, 0, {{Cast.getOperand(0), 1}})
                 : D::makeForget(&Cast);
    case llvm::Instruction::Trunc:
      return D::bitWidthOf(Cast.getOperand(0)) == D::bitWidthOf(&Cast)
                 ? D::makeAffineAssignment(&Cast, 0, {{Cast.getOperand(0), 1}})
                 : D::makeForget(&Cast);
    default:
      return D::makeForget(&Cast);
    }
  }

  Relation buildCompareRelation(const llvm::ICmpInst &Cmp) const {
    int64_t lhs = 0, rhs = 0;
    if (getConstantIntValue(Cmp.getOperand(0), lhs) &&
        getConstantIntValue(Cmp.getOperand(1), rhs)) {
      llvm::APInt lhsValue(Cmp.getOperand(0)->getType()->getIntegerBitWidth(),
                           static_cast<uint64_t>(lhs), true);
      llvm::APInt rhsValue(Cmp.getOperand(1)->getType()->getIntegerBitWidth(),
                           static_cast<uint64_t>(rhs), true);
      bool result =
          llvm::ICmpInst::compare(lhsValue, rhsValue, Cmp.getPredicate());
      return D::makeAffineAssignment(&Cmp, result ? 1 : 0, {});
    }
    if (Cmp.getOperand(0) == Cmp.getOperand(1)) {
      switch (Cmp.getPredicate()) {
      case llvm::CmpInst::ICMP_EQ:
      case llvm::CmpInst::ICMP_UGE:
      case llvm::CmpInst::ICMP_ULE:
      case llvm::CmpInst::ICMP_SGE:
      case llvm::CmpInst::ICMP_SLE:
        return D::makeAffineAssignment(&Cmp, 1, {});
      case llvm::CmpInst::ICMP_NE:
      case llvm::CmpInst::ICMP_UGT:
      case llvm::CmpInst::ICMP_ULT:
      case llvm::CmpInst::ICMP_SGT:
      case llvm::CmpInst::ICMP_SLT:
        return D::makeAffineAssignment(&Cmp, 0, {});
      default:
        break;
      }
    }
    return D::makeForget(&Cmp);
  }

  Relation buildSelectRelation(const llvm::SelectInst &Select) const {
    int64_t cond = 0;
    if (getConstantIntValue(Select.getCondition(), cond))
      return relationForValue(&Select, cond != 0 ? Select.getTrueValue()
                                                 : Select.getFalseValue());
    Relation lhs = relationForValue(&Select, Select.getTrueValue());
    Relation rhs = relationForValue(&Select, Select.getFalseValue());
    if (D::isTrackedValue(Select.getCondition())) {
      lhs = D::addPrecondition(lhs, Select.getCondition(), 1);
      rhs = D::addPrecondition(rhs, Select.getCondition(), 0);
    }
    return D::combine(lhs, rhs);
  }

  Relation buildBinaryRelation(const llvm::BinaryOperator &BinOp) const {
    auto *L = BinOp.getOperand(0);
    auto *R = BinOp.getOperand(1);
    const unsigned width = D::bitWidthOf(&BinOp);
    int64_t lhsConst = 0, rhsConst = 0;
    switch (BinOp.getOpcode()) {
    case llvm::Instruction::Add:
      if (getConstantIntValue(L, lhsConst) && getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(
            &BinOp, wrapToBitWidth(lhsConst + rhsConst, width), {});
      if (getConstantIntValue(L, lhsConst))
        return D::makeAffineAssignment(&BinOp, wrapToBitWidth(lhsConst, width),
                                       {{R, 1}});
      if (getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(&BinOp, wrapToBitWidth(rhsConst, width),
                                       {{L, 1}});
      if (D::isTrackedValue(L) && D::isTrackedValue(R))
        return D::makeAffineAssignment(&BinOp, 0, {{L, 1}, {R, 1}});
      return D::makeForget(&BinOp);
    case llvm::Instruction::Sub:
      if (getConstantIntValue(L, lhsConst) && getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(
            &BinOp, wrapToBitWidth(lhsConst - rhsConst, width), {});
      if (getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(&BinOp, wrapToBitWidth(-rhsConst, width),
                                       {{L, 1}});
      if (D::isTrackedValue(L) && D::isTrackedValue(R))
        return D::makeAffineAssignment(&BinOp, 0, {{L, 1}, {R, -1}});
      return D::makeForget(&BinOp);
    case llvm::Instruction::Mul:
      if (getConstantIntValue(L, lhsConst) && getConstantIntValue(R, rhsConst))
        return D::makeAffineAssignment(
            &BinOp, wrapToBitWidth(lhsConst * rhsConst, width), {});
      if (getConstantIntValue(L, lhsConst) && D::isTrackedValue(R))
        return D::makeAffineAssignment(&BinOp, 0,
                                       {{R, wrapToBitWidth(lhsConst, width)}});
      if (getConstantIntValue(R, rhsConst) && D::isTrackedValue(L))
        return D::makeAffineAssignment(&BinOp, 0,
                                       {{L, wrapToBitWidth(rhsConst, width)}});
      return D::makeForget(&BinOp);
    default:
      return D::makeForget(&BinOp);
    }
  }
};

} // namespace

AffineState
materializeAffineExpressions(const AffineRelationDomain::value_type &relation) {
  return materializeAffineExpressionsImpl(relation);
}

InterproceduralAffineEqualities::Result InterproceduralAffineEqualities::run(
    llvm::Module &M, bool verbose, LinearStrategy linearStrategy,
    IndirectCallResolutionMode callResolutionMode) {
  AffineRelationAnalysis analysis(M);
  auto engineResult =
      InterproceduralEngine<AffineRelationDomain, AffineRelationAnalysis>::run(
          M, analysis, verbose, linearStrategy, callResolutionMode);
  return analysis.buildResult(engineResult);
}

} // namespace npa

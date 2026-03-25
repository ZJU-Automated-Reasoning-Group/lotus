#include "FailureDirectedTrimmingImpl.h"

#include <llvm/IR/DerivedTypes.h>

using namespace llvm;

// Expression language for safety/trimming conditions.
//
// Expr is a small, typed AST used by the safety-condition analysis and the
// trimming instrumentation pipeline.
//
// Notes on normalization:
//   - ExprFactory::{and_,or_} flatten nested nodes, drop neutral elements, and
//     (for sharing) deduplicate identical children by pointer identity.
//   - exprToString additionally sorts And/Or children, which is used as a
//     pragmatic "stability" check during CFG iteration.
//
// Notes on quantified variables:
//   - BoundVarManager assigns stable numeric ids to quantifier binders, keyed
//   by
//     (instruction, tag, type). These ids are later used to map binders to
//     nondeterministic witness values during instrumentation.

namespace {

static void flatten(ExprKind K, std::vector<ExprRef> &Out, ExprRef In) {
  if (!In)
    return;
  if (In->Kind == K) {
    for (const auto &C : In->Args)
      Out.push_back(C);
    return;
  }
  Out.push_back(In);
}

} // namespace

// -----------------------------------------------------------------------------
// BoundKeyInfo
// -----------------------------------------------------------------------------
BoundKey BoundKeyInfo::getTombstoneKey() {
  return {reinterpret_cast<const Instruction *>(~uintptr_t(0)), ~uint64_t(0),
          reinterpret_cast<Type *>(~uintptr_t(0))};
}

unsigned BoundKeyInfo::getHashValue(const BoundKey &K) {
  return static_cast<unsigned>(
      llvm::hash_combine(reinterpret_cast<uintptr_t>(K.Inst), K.Tag,
                         reinterpret_cast<uintptr_t>(K.Ty)));
}

bool BoundKeyInfo::isEqual(const BoundKey &LHS, const BoundKey &RHS) {
  return LHS.Inst == RHS.Inst && LHS.Tag == RHS.Tag && LHS.Ty == RHS.Ty;
}

uint32_t BoundVarManager::getId(const Instruction *I, uint64_t Tag, Type *Ty) {
  // Returns a stable id for a (binder) variable identified by the IR context.
  // Stability matters because we later refer to the same binder by id when
  // eliminating quantifiers and generating LLVM IR.
  BoundKey K{I, Tag, Ty};
  auto It = Ids.find(K);
  if (It != Ids.end())
    return It->second;
  uint32_t Fresh = NextId++;
  Ids[K] = Fresh;
  return Fresh;
}

// -----------------------------------------------------------------------------
// ExprFactory
// -----------------------------------------------------------------------------
ExprRef ExprFactory::boolConst(bool V) const {
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::BoolConst;
  E->Ty = Type::getInt1Ty(Ctx);
  E->BoolVal = V;
  return E;
}

ExprRef ExprFactory::intConst(const APInt &V, IntegerType *Ty) const {
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::IntConst;
  E->Ty = Ty;
  E->IntVal = V;
  return E;
}

ExprRef ExprFactory::var(const Value *V) const {
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Var;
  E->Ty = V->getType();
  E->VarVal = V;
  return E;
}

ExprRef ExprFactory::boundVar(uint32_t Id, Type *Ty) const {
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::BoundVar;
  E->Ty = Ty;
  E->BoundId = Id;
  return E;
}

ExprRef ExprFactory::not_(ExprRef A) const {
  if (!A)
    return nullptr;
  if (A->Kind == ExprKind::BoolConst)
    return boolConst(!A->BoolVal);
  if (A->Kind == ExprKind::Not)
    return A->Args.empty() ? nullptr : A->Args[0];
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Not;
  E->Ty = Type::getInt1Ty(Ctx);
  E->Args = {A};
  return E;
}

ExprRef ExprFactory::and_(ArrayRef<ExprRef> Children) const {
  std::vector<ExprRef> Flat;
  Flat.reserve(Children.size());
  for (auto &C : Children)
    flatten(ExprKind::And, Flat, C);

  std::vector<ExprRef> Norm;
  Norm.reserve(Flat.size());
  for (auto &C : Flat) {
    if (!C)
      continue;
    if (C->Kind == ExprKind::BoolConst) {
      if (!C->BoolVal)
        return boolConst(false);
      continue;
    }
    Norm.push_back(C);
  }

  if (Norm.empty())
    return boolConst(true);
  if (Norm.size() == 1)
    return Norm[0];

  std::sort(Norm.begin(), Norm.end(), [](const ExprRef &A, const ExprRef &B) {
    return A.get() < B.get();
  });
  Norm.erase(std::unique(Norm.begin(), Norm.end(),
                         [](const ExprRef &A, const ExprRef &B) {
                           return A.get() == B.get();
                         }),
             Norm.end());

  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::And;
  E->Ty = Type::getInt1Ty(Ctx);
  E->Args = std::move(Norm);
  return E;
}

ExprRef ExprFactory::or_(ArrayRef<ExprRef> Children) const {
  std::vector<ExprRef> Flat;
  Flat.reserve(Children.size());
  for (auto &C : Children)
    flatten(ExprKind::Or, Flat, C);

  std::vector<ExprRef> Norm;
  Norm.reserve(Flat.size());
  for (auto &C : Flat) {
    if (!C)
      continue;
    if (C->Kind == ExprKind::BoolConst) {
      if (C->BoolVal)
        return boolConst(true);
      continue;
    }
    Norm.push_back(C);
  }

  if (Norm.empty())
    return boolConst(false);
  if (Norm.size() == 1)
    return Norm[0];

  std::sort(Norm.begin(), Norm.end(), [](const ExprRef &A, const ExprRef &B) {
    return A.get() < B.get();
  });
  Norm.erase(std::unique(Norm.begin(), Norm.end(),
                         [](const ExprRef &A, const ExprRef &B) {
                           return A.get() == B.get();
                         }),
             Norm.end());

  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Or;
  E->Ty = Type::getInt1Ty(Ctx);
  E->Args = std::move(Norm);
  return E;
}

ExprRef ExprFactory::add(ExprRef A, ExprRef B) const {
  if (!A || !B)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Add;
  E->Ty = A->Ty;
  E->Args = {A, B};
  return E;
}

ExprRef ExprFactory::sub(ExprRef A, ExprRef B) const {
  if (!A || !B)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Sub;
  E->Ty = A->Ty;
  E->Args = {A, B};
  return E;
}

ExprRef ExprFactory::mul(ExprRef A, ExprRef B) const {
  if (!A || !B)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Mul;
  E->Ty = A->Ty;
  E->Args = {A, B};
  return E;
}

static ExprRef mkBin(ExprKind K, ExprRef A, ExprRef B) {
  if (!A || !B)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = K;
  E->Ty = A->Ty;
  E->Args = {A, B};
  return E;
}

ExprRef ExprFactory::band(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::BAnd, A, B);
}

ExprRef ExprFactory::bor(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::BOr, A, B);
}

ExprRef ExprFactory::bxor(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::BXor, A, B);
}

ExprRef ExprFactory::shl(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::Shl, A, B);
}

ExprRef ExprFactory::lshr(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::LShr, A, B);
}

ExprRef ExprFactory::ashr(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::AShr, A, B);
}

ExprRef ExprFactory::udiv(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::UDiv, A, B);
}

ExprRef ExprFactory::sdiv(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::SDiv, A, B);
}

ExprRef ExprFactory::urem(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::URem, A, B);
}

ExprRef ExprFactory::srem(ExprRef A, ExprRef B) const {
  return mkBin(ExprKind::SRem, A, B);
}

ExprRef ExprFactory::icmp(CmpInst::Predicate P, ExprRef A, ExprRef B) const {
  if (!A || !B)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::ICmp;
  E->Ty = Type::getInt1Ty(Ctx);
  E->Pred = P;
  E->Args = {A, B};
  return E;
}

ExprRef ExprFactory::implies(ExprRef Cond, ExprRef Then) const {
  return or_({not_(Cond), Then});
}

ExprRef ExprFactory::select(ExprRef Cond, ExprRef T, ExprRef F) const {
  if (!Cond || !T || !F)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Select;
  E->Ty = T->Ty;
  E->Args = {Cond, T, F};
  return E;
}

ExprRef ExprFactory::deref(ExprRef Ptr, Type *ValueTy) const {
  if (!Ptr || !ValueTy)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Deref;
  E->Ty = ValueTy;
  E->DerefValueTy = ValueTy;
  E->Args = {Ptr};
  return E;
}

ExprRef ExprFactory::cast(Instruction::CastOps Op, Type *DstTy,
                          ExprRef Src) const {
  if (!Src || !DstTy)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Cast;
  E->Ty = DstTy;
  E->CastOp = Op;
  E->Args = {Src};
  return E;
}

ExprRef ExprFactory::gep(Type *SourceEltTy, bool InBounds, ExprRef BasePtr,
                         ArrayRef<ExprRef> Indices, Type *ResultTy) const {
  if (!BasePtr || !ResultTy || !SourceEltTy)
    return nullptr;
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Gep;
  E->Ty = ResultTy;
  E->GepSourceEltTy = SourceEltTy;
  E->GepInBounds = InBounds;
  E->Args.reserve(1 + Indices.size());
  E->Args.push_back(BasePtr);
  for (auto &Idx : Indices)
    E->Args.push_back(Idx);
  return E;
}

ExprRef ExprFactory::forall(uint32_t Id, Type *Ty, ExprRef Body) const {
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Forall;
  E->Ty = Type::getInt1Ty(Ctx);
  E->BoundId = Id;
  E->DerefValueTy = Ty;
  E->Args = {Body};
  return E;
}

ExprRef ExprFactory::exists(uint32_t Id, Type *Ty, ExprRef Body) const {
  auto E = std::make_shared<Expr>();
  E->Kind = ExprKind::Exists;
  E->Ty = Type::getInt1Ty(Ctx);
  E->BoundId = Id;
  E->DerefValueTy = Ty;
  E->Args = {Body};
  return E;
}

// -----------------------------------------------------------------------------
// exprToString, exprEquals, exprContainsKind, isNullPtrExpr
// -----------------------------------------------------------------------------
std::string exprToString(const ExprRef &E) {
  if (!E)
    return "<null>";
  switch (E->Kind) {
  case ExprKind::BoolConst:
    return E->BoolVal ? "true" : "false";
  case ExprKind::IntConst: {
    std::string S;
    raw_string_ostream OS(S);
    OS << E->IntVal;
    return OS.str();
  }
  case ExprKind::Var:
    if (E->VarVal && E->VarVal->hasName())
      return ("%" + E->VarVal->getName()).str();
    return ("<var@" + std::to_string(reinterpret_cast<uintptr_t>(E->VarVal)) +
            ">");
  case ExprKind::BoundVar:
    return ("$b" + std::to_string(E->BoundId));
  case ExprKind::Not:
    return "(!" + exprToString(E->Args[0]) + ")";
  case ExprKind::And: {
    std::vector<std::string> Parts;
    Parts.reserve(E->Args.size());
    for (auto &C : E->Args)
      Parts.push_back(exprToString(C));
    std::sort(Parts.begin(), Parts.end());
    std::string S = "(";
    for (size_t i = 0; i < Parts.size(); ++i) {
      if (i)
        S += " && ";
      S += Parts[i];
    }
    S += ")";
    return S;
  }
  case ExprKind::Or: {
    std::vector<std::string> Parts;
    Parts.reserve(E->Args.size());
    for (auto &C : E->Args)
      Parts.push_back(exprToString(C));
    std::sort(Parts.begin(), Parts.end());
    std::string S = "(";
    for (size_t i = 0; i < Parts.size(); ++i) {
      if (i)
        S += " || ";
      S += Parts[i];
    }
    S += ")";
    return S;
  }
  case ExprKind::Add:
    return "(" + exprToString(E->Args[0]) + " + " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::Sub:
    return "(" + exprToString(E->Args[0]) + " - " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::Mul:
    return "(" + exprToString(E->Args[0]) + " * " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::BAnd:
    return "(" + exprToString(E->Args[0]) + " & " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::BOr:
    return "(" + exprToString(E->Args[0]) + " | " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::BXor:
    return "(" + exprToString(E->Args[0]) + " ^ " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::Shl:
    return "(" + exprToString(E->Args[0]) + " << " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::LShr:
    return "(" + exprToString(E->Args[0]) + " l>> " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::AShr:
    return "(" + exprToString(E->Args[0]) + " a>> " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::UDiv:
    return "(" + exprToString(E->Args[0]) + " u/ " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::SDiv:
    return "(" + exprToString(E->Args[0]) + " s/ " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::URem:
    return "(" + exprToString(E->Args[0]) + " u% " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::SRem:
    return "(" + exprToString(E->Args[0]) + " s% " + exprToString(E->Args[1]) +
           ")";
  case ExprKind::ICmp: {
    std::string Op = CmpInst::getPredicateName(E->Pred).str();
    return "(" + exprToString(E->Args[0]) + " " + Op + " " +
           exprToString(E->Args[1]) + ")";
  }
  case ExprKind::Select:
    return "(select " + exprToString(E->Args[0]) + " ? " +
           exprToString(E->Args[1]) + " : " + exprToString(E->Args[2]) + ")";
  case ExprKind::Deref:
    return "(*" + exprToString(E->Args[0]) + ")";
  case ExprKind::Cast:
    return "(cast " + std::to_string(static_cast<unsigned>(E->CastOp)) + " " +
           exprToString(E->Args[0]) + ")";
  case ExprKind::Gep: {
    std::string S = "(gep " + exprToString(E->Args[0]);
    for (size_t i = 1; i < E->Args.size(); ++i)
      S += (", " + exprToString(E->Args[i]));
    S += ")";
    return S;
  }
  case ExprKind::Forall:
    return "(forall $" + std::to_string(E->BoundId) + ". " +
           exprToString(E->Args[0]) + ")";
  case ExprKind::Exists:
    return "(exists $" + std::to_string(E->BoundId) + ". " +
           exprToString(E->Args[0]) + ")";
  }
  return "<unknown>";
}

bool exprEquals(const ExprRef &A, const ExprRef &B) {
  if (A.get() == B.get())
    return true;
  if (!A || !B)
    return false;
  if (A->Kind != B->Kind)
    return false;
  if (A->Ty != B->Ty)
    return false;
  if (A->Args.size() != B->Args.size())
    return false;

  switch (A->Kind) {
  case ExprKind::BoolConst:
    return A->BoolVal == B->BoolVal;
  case ExprKind::IntConst:
    return A->IntVal == B->IntVal;
  case ExprKind::Var:
    return A->VarVal == B->VarVal;
  case ExprKind::BoundVar:
    return A->BoundId == B->BoundId;
  case ExprKind::Not:
  case ExprKind::And:
  case ExprKind::Or:
  case ExprKind::Add:
  case ExprKind::Sub:
  case ExprKind::Mul:
  case ExprKind::BAnd:
  case ExprKind::BOr:
  case ExprKind::BXor:
  case ExprKind::Shl:
  case ExprKind::LShr:
  case ExprKind::AShr:
  case ExprKind::UDiv:
  case ExprKind::SDiv:
  case ExprKind::URem:
  case ExprKind::SRem:
    break;
  case ExprKind::ICmp:
    if (A->Pred != B->Pred)
      return false;
    break;
  case ExprKind::Select:
    break;
  case ExprKind::Deref:
    if (A->DerefValueTy != B->DerefValueTy)
      return false;
    break;
  case ExprKind::Cast:
    if (A->CastOp != B->CastOp)
      return false;
    break;
  case ExprKind::Gep:
    if (A->GepSourceEltTy != B->GepSourceEltTy ||
        A->GepInBounds != B->GepInBounds)
      return false;
    break;
  case ExprKind::Forall:
  case ExprKind::Exists:
    if (A->BoundId != B->BoundId || A->DerefValueTy != B->DerefValueTy)
      return false;
    break;
  }

  for (size_t i = 0; i < A->Args.size(); ++i) {
    if (!exprEquals(A->Args[i], B->Args[i]))
      return false;
  }
  return true;
}

bool exprContainsKind(const ExprRef &E, ExprKind K) {
  if (!E)
    return false;
  if (E->Kind == K)
    return true;
  for (auto &C : E->Args) {
    if (exprContainsKind(C, K))
      return true;
  }
  return false;
}

bool isNullPtrExpr(const ExprRef &E) {
  if (!E || E->Kind != ExprKind::Var || !E->Ty || !E->Ty->isPointerTy())
    return false;
  const auto *C = dyn_cast_or_null<Constant>(E->VarVal);
  return C && C->isNullValue();
}

bool mayAliasPtrExpr(const ExprRef &A, const ExprRef &B,
                     lotus::AliasAnalysisWrapper &AA) {
  if (!A || !B)
    return true;
  if (!A->Ty || !B->Ty || !A->Ty->isPointerTy() || !B->Ty->isPointerTy())
    return true;

  if (isNullPtrExpr(A) || isNullPtrExpr(B))
    return isNullPtrExpr(A) && isNullPtrExpr(B);

  if (exprEquals(A, B))
    return true;

  if (A->Kind == ExprKind::Var && B->Kind == ExprKind::Var)
    return AA.mayAlias(A->VarVal, B->VarVal);

  if (exprContainsKind(A, ExprKind::Deref) ||
      exprContainsKind(B, ExprKind::Deref) ||
      exprContainsKind(A, ExprKind::BoundVar) ||
      exprContainsKind(B, ExprKind::BoundVar))
    return true;

  if (A->Kind == ExprKind::Cast && A->CastOp == Instruction::BitCast)
    return mayAliasPtrExpr(A->Args[0], B, AA);
  if (B->Kind == ExprKind::Cast && B->CastOp == Instruction::BitCast)
    return mayAliasPtrExpr(A, B->Args[0], AA);

  return true;
}

// -----------------------------------------------------------------------------
// substitute (with memo), collectDeref*, collectPointerVars, negate,
// boundConjuncts
// -----------------------------------------------------------------------------
static ExprRef substituteImpl(const ExprFactory &F, const ExprRef &E,
                              const Subst &S,
                              DenseMap<const Expr *, ExprRef> &Memo) {
  if (!E)
    return nullptr;

  auto It = Memo.find(E.get());
  if (It != Memo.end())
    return It->second;

  ExprRef Out = E;
  switch (E->Kind) {
  case ExprKind::Var: {
    auto I = S.Vars.find(E->VarVal);
    if (I != S.Vars.end())
      Out = I->second;
    break;
  }
  case ExprKind::BoundVar: {
    auto I = S.Bound.find(E->BoundId);
    if (I != S.Bound.end())
      Out = I->second;
    break;
  }
  case ExprKind::BoolConst:
  case ExprKind::IntConst:
    break;
  case ExprKind::Not: {
    DenseMap<const Expr *, ExprRef> Memo2;
    ExprRef A = substituteImpl(F, E->Args[0], S, Memo2);
    Out = F.not_(A);
    break;
  }
  case ExprKind::And:
  case ExprKind::Or: {
    std::vector<ExprRef> Kids;
    Kids.reserve(E->Args.size());
    for (auto &C : E->Args) {
      DenseMap<const Expr *, ExprRef> Memo2;
      Kids.push_back(substituteImpl(F, C, S, Memo2));
    }
    Out = (E->Kind == ExprKind::And) ? F.and_(Kids) : F.or_(Kids);
    break;
  }
  case ExprKind::Add:
  case ExprKind::Sub:
  case ExprKind::Mul:
  case ExprKind::BAnd:
  case ExprKind::BOr:
  case ExprKind::BXor:
  case ExprKind::Shl:
  case ExprKind::LShr:
  case ExprKind::AShr:
  case ExprKind::UDiv:
  case ExprKind::SDiv:
  case ExprKind::URem:
  case ExprKind::SRem:
  case ExprKind::ICmp:
  case ExprKind::Select:
  case ExprKind::Deref:
  case ExprKind::Cast:
  case ExprKind::Gep: {
    std::vector<ExprRef> Kids;
    Kids.reserve(E->Args.size());
    for (auto &C : E->Args) {
      DenseMap<const Expr *, ExprRef> Memo2;
      Kids.push_back(substituteImpl(F, C, S, Memo2));
    }
    if (E->Kind == ExprKind::Add)
      Out = F.add(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::Sub)
      Out = F.sub(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::Mul)
      Out = F.mul(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::BAnd)
      Out = F.band(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::BOr)
      Out = F.bor(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::BXor)
      Out = F.bxor(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::Shl)
      Out = F.shl(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::LShr)
      Out = F.lshr(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::AShr)
      Out = F.ashr(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::UDiv)
      Out = F.udiv(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::SDiv)
      Out = F.sdiv(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::URem)
      Out = F.urem(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::SRem)
      Out = F.srem(Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::ICmp)
      Out = F.icmp(E->Pred, Kids[0], Kids[1]);
    else if (E->Kind == ExprKind::Select)
      Out = F.select(Kids[0], Kids[1], Kids[2]);
    else if (E->Kind == ExprKind::Deref)
      Out = F.deref(Kids[0], E->DerefValueTy);
    else if (E->Kind == ExprKind::Cast)
      Out = F.cast(E->CastOp, E->Ty, Kids[0]);
    else if (E->Kind == ExprKind::Gep) {
      SmallVector<ExprRef, 8> Idxs;
      for (size_t i = 1; i < Kids.size(); ++i)
        Idxs.push_back(Kids[i]);
      Out = F.gep(E->GepSourceEltTy, E->GepInBounds, Kids[0], Idxs, E->Ty);
    }
    break;
  }
  case ExprKind::Forall:
  case ExprKind::Exists: {
    DenseMap<const Expr *, ExprRef> Memo2;
    ExprRef Body = substituteImpl(F, E->Args[0], S, Memo2);
    Out = (E->Kind == ExprKind::Forall)
              ? F.forall(E->BoundId, E->DerefValueTy, Body)
              : F.exists(E->BoundId, E->DerefValueTy, Body);
    break;
  }
  }

  Memo[E.get()] = Out;
  return Out;
}

ExprRef substitute(const ExprFactory &F, const ExprRef &E, const Subst &S) {
  DenseMap<const Expr *, ExprRef> Memo;
  return substituteImpl(F, E, S, Memo);
}

void collectDerefPtrs(const ExprRef &E, std::vector<ExprRef> &Out) {
  if (!E)
    return;
  if (E->Kind == ExprKind::Deref) {
    if (!E->Args.empty())
      Out.push_back(E->Args[0]);
  }
  for (auto &C : E->Args)
    collectDerefPtrs(C, Out);
}

void collectDerefNodes(const ExprRef &E, std::vector<ExprRef> &Out) {
  if (!E)
    return;
  if (E->Kind == ExprKind::Deref)
    Out.push_back(E);
  for (auto &C : E->Args)
    collectDerefNodes(C, Out);
}

void collectPointerVars(const ExprRef &E, SmallVectorImpl<const Value *> &Out) {
  if (!E)
    return;
  if (E->Kind == ExprKind::Var && E->VarVal && E->Ty && E->Ty->isPointerTy()) {
    Out.push_back(E->VarVal);
  }
  for (auto &C : E->Args)
    collectPointerVars(C, Out);
}

// Paper §5: trimming condition = ¬(safety condition). De Morgan for ∧/∨;
// quantifier flip: ¬∀x.φ → ∃x.¬φ, ¬∃x.φ → ∀x.¬φ (safety uses ∀ for havoc;
// negation yields ∃).
ExprRef negateForTrimming(const ExprFactory &F, const ExprRef &E) {
  // Turn SC(π) into TC(π) = ¬SC(π). Existentials must be eliminated before
  // codegen (QE or nondet).
  if (!E)
    return nullptr;
  switch (E->Kind) {
  case ExprKind::BoolConst:
    return F.boolConst(!E->BoolVal);
  case ExprKind::Not:
    return E->Args.empty() ? nullptr : E->Args[0];
  case ExprKind::And: {
    std::vector<ExprRef> Kids;
    Kids.reserve(E->Args.size());
    for (auto &C : E->Args)
      Kids.push_back(negateForTrimming(F, C));
    return F.or_(Kids);
  }
  case ExprKind::Or: {
    std::vector<ExprRef> Kids;
    Kids.reserve(E->Args.size());
    for (auto &C : E->Args)
      Kids.push_back(negateForTrimming(F, C));
    return F.and_(Kids);
  }
  case ExprKind::Forall:
    return F.exists(E->BoundId, E->DerefValueTy,
                    negateForTrimming(F, E->Args[0]));
  case ExprKind::Exists:
    return F.forall(E->BoundId, E->DerefValueTy,
                    negateForTrimming(F, E->Args[0]));
  default:
    return F.not_(E);
  }
}

// Paper §6 Bounding the instrumentation: cap conjuncts so simplified formula is
// weaker than original (subset of conjuncts ⇒ weaker trimming condition ⇒ prune
// fewer paths, still sound).
ExprRef boundConjuncts(const ExprFactory &F, const ExprRef &E, unsigned Max) {
  // Keep at most Max conjuncts (lex order of exprToString). Smaller assumes,
  // less pruning.
  if (!E || Max == 0)
    return E;
  if (E->Kind != ExprKind::And)
    return E;
  if (E->Args.size() <= Max)
    return E;

  std::vector<std::pair<std::string, ExprRef>> Pairs;
  Pairs.reserve(E->Args.size());
  for (auto &C : E->Args)
    Pairs.emplace_back(exprToString(C), C);
  std::sort(Pairs.begin(), Pairs.end(),
            [](auto &A, auto &B) { return A.first < B.first; });

  std::vector<ExprRef> Keep;
  Keep.reserve(Max);
  for (unsigned i = 0; i < Max; ++i)
    Keep.push_back(Pairs[i].second);
  return F.and_(Keep);
}

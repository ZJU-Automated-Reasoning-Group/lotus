// Optional quantifier elimination for trimming conditions (paper §6 Eliminating
// quantifiers). Negation of safety conditions yields ∃ from ∀ (havoc); Z3 QE
// eliminates existentials to reduce nondeterminism in inserted assume
// conditions.
#include "FailureDirectedTrimmingImpl.h"

#include <stdexcept>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <z3++.h>

using namespace llvm;

// Z3-based QE: eliminate exists binders from trimming conditions (paper §6,
// first alternative).

namespace {

enum class Z3IntSemanticsKind { BV, Math };

static Z3IntSemanticsKind getZ3IntSemantics() {
  if (FDTrimIntSemantics == "math")
    return Z3IntSemanticsKind::Math;
  return Z3IntSemanticsKind::BV;
}

struct Z3UFInfo {
  enum class Kind { Drf, Cast, Gep };
  Kind K;
  Type *RetTy = nullptr;
  Instruction::CastOps CastOp = Instruction::BitCast;
  Type *CastDstTy = nullptr;
  Type *GepSourceEltTy = nullptr;
  bool GepInBounds = false;
  Type *GepResultTy = nullptr;
  unsigned GepNumIdx = 0;
};

struct Z3QESession {
  const ExprFactory &F;
  Module &M;
  z3::context Ctx;
  z3::sort PtrSort;
  Z3IntSemanticsKind IntSem;

  std::unordered_map<const Expr *, z3::expr> Memo;
  std::unordered_map<std::string, z3::expr> NamedConsts;
  std::unordered_map<std::string, ExprRef> NameToExpr;
  std::unordered_map<uint32_t, z3::expr> BoundConsts;

  std::unordered_map<std::string, z3::func_decl> UFDecls;
  std::unordered_map<std::string, Z3UFInfo> UFInfo;

  explicit Z3QESession(const ExprFactory &EF, Module &Mod, Z3IntSemanticsKind S)
      : F(EF), M(Mod), Ctx(), PtrSort(Ctx.uninterpreted_sort("Ptr")),
        IntSem(S) {}

  z3::sort sortOf(Type *Ty) {
    if (!Ty)
      throw std::runtime_error("null type");
    if (Ty->isIntegerTy(1))
      return Ctx.bool_sort();
    if (auto *IT = dyn_cast<IntegerType>(Ty)) {
      if (IntSem == Z3IntSemanticsKind::Math)
        return Ctx.int_sort();
      return Ctx.bv_sort(IT->getBitWidth());
    }
    if (Ty->isPointerTy())
      return PtrSort;
    throw std::runtime_error("unsupported sort");
  }

  z3::expr mkNumeral(const APInt &V, Type *Ty) {
    if (auto *IT = dyn_cast<IntegerType>(Ty)) {
      std::string S;
      raw_string_ostream OS(S);
      OS << V;
      OS.flush();
      if (IntSem == Z3IntSemanticsKind::Math)
        return Ctx.int_val(S.c_str());
      return Ctx.bv_val(S.c_str(), IT->getBitWidth());
    }
    throw std::runtime_error("unsupported numeral");
  }

  std::string nameForValue(const Value *V) {
    std::string Name = "v" + std::to_string(reinterpret_cast<uintptr_t>(V));
    if (V && V->hasName())
      Name += ("_" + V->getName()).str();
    return Name;
  }

  z3::expr varForValue(const Value *V) {
    if (!V)
      throw std::runtime_error("null value");
    std::string N = nameForValue(V);
    auto It = NamedConsts.find(N);
    if (It != NamedConsts.end())
      return It->second;
    z3::sort S = sortOf(V->getType());
    z3::expr C = Ctx.constant(N.c_str(), S);
    NamedConsts.emplace(N, C);
    NameToExpr.emplace(N, F.var(V));
    return C;
  }

  z3::func_decl uf(const std::string &Name, z3::sort Ret,
                   const std::vector<z3::sort> &Args, const Z3UFInfo &Info) {
    auto It = UFDecls.find(Name);
    if (It != UFDecls.end())
      return It->second;
    z3::func_decl D =
        Ctx.function(Name.c_str(), static_cast<unsigned>(Args.size()),
                     const_cast<z3::sort *>(Args.data()), Ret);
    UFDecls.emplace(Name, D);
    UFInfo.emplace(Name, Info);
    return D;
  }

  z3::expr toZ3(const ExprRef &E) {
    if (!E)
      throw std::runtime_error("null expr");
    auto It = Memo.find(E.get());
    if (It != Memo.end())
      return It->second;

    z3::expr Out = Ctx.bool_val(true);
    switch (E->Kind) {
    case ExprKind::BoolConst:
      Out = Ctx.bool_val(E->BoolVal);
      break;
    case ExprKind::IntConst:
      Out = mkNumeral(E->IntVal, E->Ty);
      break;
    case ExprKind::Var:
      Out = varForValue(E->VarVal);
      break;
    case ExprKind::BoundVar: {
      auto BI = BoundConsts.find(E->BoundId);
      if (BI == BoundConsts.end())
        throw std::runtime_error("unbound bound-var");
      Out = BI->second;
      break;
    }
    case ExprKind::Not:
      Out = !toZ3(E->Args[0]);
      break;
    case ExprKind::And: {
      z3::expr Acc = Ctx.bool_val(true);
      for (auto &C : E->Args)
        Acc = Acc && toZ3(C);
      Out = Acc;
      break;
    }
    case ExprKind::Or: {
      z3::expr Acc = Ctx.bool_val(false);
      for (auto &C : E->Args)
        Acc = Acc || toZ3(C);
      Out = Acc;
      break;
    }
    case ExprKind::Add:
      Out = toZ3(E->Args[0]) + toZ3(E->Args[1]);
      break;
    case ExprKind::Sub:
      Out = toZ3(E->Args[0]) - toZ3(E->Args[1]);
      break;
    case ExprKind::Mul:
      Out = toZ3(E->Args[0]) * toZ3(E->Args[1]);
      break;
    case ExprKind::BAnd: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.band." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast; // UF is never decoded in math mode
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvand(Ctx, A, B));
      }
      break;
    }
    case ExprKind::BOr: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.bor." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvor(Ctx, A, B));
      }
      break;
    }
    case ExprKind::BXor: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.bxor." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvxor(Ctx, A, B));
      }
      break;
    }
    case ExprKind::Shl: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.shl." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvshl(Ctx, A, B));
      }
      break;
    }
    case ExprKind::LShr: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.lshr." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvlshr(Ctx, A, B));
      }
      break;
    }
    case ExprKind::AShr: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.ashr." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvashr(Ctx, A, B));
      }
      break;
    }
    case ExprKind::UDiv: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.udiv." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvudiv(Ctx, A, B));
      }
      break;
    }
    case ExprKind::SDiv: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.sdiv." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvsdiv(Ctx, A, B));
      }
      break;
    }
    case ExprKind::URem: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.urem." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvurem(Ctx, A, B));
      }
      break;
    }
    case ExprKind::SRem: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (IntSem == Z3IntSemanticsKind::Math) {
        std::string Name =
            "fdtrim.srem." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
        Z3UFInfo Info;
        Info.K = Z3UFInfo::Kind::Cast;
        z3::func_decl D =
            uf(Name, sortOf(E->Ty), {sortOf(E->Ty), sortOf(E->Ty)}, Info);
        Out = D(A, B);
      } else {
        Out = z3::to_expr(Ctx, Z3_mk_bvsrem(Ctx, A, B));
      }
      break;
    }
    case ExprKind::ICmp: {
      z3::expr A = toZ3(E->Args[0]);
      z3::expr B = toZ3(E->Args[1]);
      if (E->Args[0]->Ty && E->Args[0]->Ty->isPointerTy()) {
        if (E->Pred == CmpInst::ICMP_EQ)
          Out = (A == B);
        else if (E->Pred == CmpInst::ICMP_NE)
          Out = (A != B);
        else
          throw std::runtime_error("unsupported pointer icmp");
        break;
      }
      if (IntSem == Z3IntSemanticsKind::Math) {
        switch (E->Pred) {
        case CmpInst::ICMP_EQ:
          Out = (A == B);
          break;
        case CmpInst::ICMP_NE:
          Out = (A != B);
          break;
        case CmpInst::ICMP_SLT:
        case CmpInst::ICMP_ULT:
          Out = (A < B);
          break;
        case CmpInst::ICMP_SLE:
        case CmpInst::ICMP_ULE:
          Out = (A <= B);
          break;
        case CmpInst::ICMP_SGT:
        case CmpInst::ICMP_UGT:
          Out = (A > B);
          break;
        case CmpInst::ICMP_SGE:
        case CmpInst::ICMP_UGE:
          Out = (A >= B);
          break;
        default:
          throw std::runtime_error("unsupported icmp");
        }
        break;
      }
      auto mkBV2 = [&](Z3_ast Ast) { return z3::to_expr(Ctx, Ast); };
      auto mkBVcmp = [&](CmpInst::Predicate P, const z3::expr &X,
                         const z3::expr &Y) -> z3::expr {
        switch (P) {
        case CmpInst::ICMP_EQ:
          return X == Y;
        case CmpInst::ICMP_NE:
          return X != Y;
        case CmpInst::ICMP_ULT:
          return mkBV2(Z3_mk_bvult(Ctx, X, Y));
        case CmpInst::ICMP_ULE:
          return mkBV2(Z3_mk_bvule(Ctx, X, Y));
        case CmpInst::ICMP_UGT:
          return mkBV2(Z3_mk_bvugt(Ctx, X, Y));
        case CmpInst::ICMP_UGE:
          return mkBV2(Z3_mk_bvuge(Ctx, X, Y));
        case CmpInst::ICMP_SLT:
          return mkBV2(Z3_mk_bvslt(Ctx, X, Y));
        case CmpInst::ICMP_SLE:
          return mkBV2(Z3_mk_bvsle(Ctx, X, Y));
        case CmpInst::ICMP_SGT:
          return mkBV2(Z3_mk_bvsgt(Ctx, X, Y));
        case CmpInst::ICMP_SGE:
          return mkBV2(Z3_mk_bvsge(Ctx, X, Y));
        default:
          throw std::runtime_error("unsupported bv icmp");
        }
      };
      Out = mkBVcmp(E->Pred, A, B);
      break;
    }
    case ExprKind::Select: {
      z3::expr Cond = toZ3(E->Args[0]);
      z3::expr T = toZ3(E->Args[1]);
      z3::expr Fv = toZ3(E->Args[2]);
      Out = z3::ite(Cond, T, Fv);
      break;
    }
    case ExprKind::Deref: {
      std::string Name =
          "fdtrim.drf." +
          std::to_string(reinterpret_cast<uintptr_t>(E->DerefValueTy));
      Z3UFInfo Info;
      Info.K = Z3UFInfo::Kind::Drf;
      Info.RetTy = E->DerefValueTy;
      z3::func_decl D = uf(Name, sortOf(E->DerefValueTy), {PtrSort}, Info);
      Out = D(toZ3(E->Args[0]));
      break;
    }
    case ExprKind::Cast: {
      std::string Name =
          "fdtrim.cast." + std::to_string(static_cast<unsigned>(E->CastOp)) +
          "." + std::to_string(reinterpret_cast<uintptr_t>(E->Ty));
      Z3UFInfo Info;
      Info.K = Z3UFInfo::Kind::Cast;
      Info.RetTy = E->Ty;
      Info.CastOp = E->CastOp;
      Info.CastDstTy = E->Ty;
      z3::func_decl D = uf(Name, sortOf(E->Ty), {sortOf(E->Args[0]->Ty)}, Info);
      Out = D(toZ3(E->Args[0]));
      break;
    }
    case ExprKind::Gep: {
      std::string Name =
          "fdtrim.gep." +
          std::to_string(reinterpret_cast<uintptr_t>(E->GepSourceEltTy)) + "." +
          std::to_string(reinterpret_cast<uintptr_t>(E->Ty)) + "." +
          std::to_string(E->GepInBounds) + "." + std::to_string(E->Args.size());
      Z3UFInfo Info;
      Info.K = Z3UFInfo::Kind::Gep;
      Info.RetTy = E->Ty;
      Info.GepSourceEltTy = E->GepSourceEltTy;
      Info.GepInBounds = E->GepInBounds;
      Info.GepResultTy = E->Ty;
      Info.GepNumIdx = E->Args.size() - 1;
      std::vector<z3::sort> ArgSorts;
      ArgSorts.reserve(E->Args.size());
      for (auto &A : E->Args)
        ArgSorts.push_back(sortOf(A->Ty));
      z3::func_decl D = uf(Name, PtrSort, ArgSorts, Info);
      z3::expr_vector Args(Ctx);
      for (auto &A : E->Args)
        Args.push_back(toZ3(A));
      Out = D(Args);
      break;
    }
    case ExprKind::Forall:
    case ExprKind::Exists: {
      std::string N = "b" + std::to_string(E->BoundId);
      z3::expr BV = Ctx.constant(N.c_str(), sortOf(E->DerefValueTy));
      auto Old = BoundConsts.find(E->BoundId);
      bool HadOld = Old != BoundConsts.end();
      z3::expr OldV = HadOld ? Old->second : Ctx.bool_val(true);
      if (HadOld)
        Old->second = BV;
      else
        BoundConsts.emplace(E->BoundId, BV);
      z3::expr Body = toZ3(E->Args[0]);
      if (HadOld)
        BoundConsts.find(E->BoundId)->second = OldV;
      else
        BoundConsts.erase(E->BoundId);
      z3::expr_vector Vars(Ctx);
      Vars.push_back(BV);
      Out = (E->Kind == ExprKind::Forall) ? z3::forall(Vars, Body)
                                          : z3::exists(Vars, Body);
      break;
    }
    }

    Memo.emplace(E.get(), Out);
    return Out;
  }

  ExprRef ptrIcmp(CmpInst::Predicate Pred, ExprRef A, ExprRef B) {
    if (!A || !B || !A->Ty || !B->Ty)
      return nullptr;
    if (!A->Ty->isPointerTy() || !B->Ty->isPointerTy() || A->Ty == B->Ty)
      return F.icmp(Pred, A, B);
    auto *PTA = cast<PointerType>(A->Ty);
    auto *PTB = cast<PointerType>(B->Ty);
    if (PTA->getAddressSpace() != PTB->getAddressSpace()) {
      if (Pred == CmpInst::ICMP_EQ)
        return F.boolConst(false);
      if (Pred == CmpInst::ICMP_NE)
        return F.boolConst(true);
      return nullptr;
    }
    Type *I8PtrTy = Type::getInt8PtrTy(F.Ctx, PTA->getAddressSpace());
    ExprRef A2 = F.cast(Instruction::BitCast, I8PtrTy, A);
    ExprRef B2 = F.cast(Instruction::BitCast, I8PtrTy, B);
    return F.icmp(Pred, A2, B2);
  }

  ExprRef fromZ3(const z3::expr &E) {
    if (E.is_true())
      return F.boolConst(true);
    if (E.is_false())
      return F.boolConst(false);

    Z3_ast_kind AK = Z3_get_ast_kind(Ctx, E);
    if (AK == Z3_NUMERAL_AST) {
      Z3_sort S = Z3_get_sort(Ctx, E);
      Z3_sort_kind SK = Z3_get_sort_kind(Ctx, S);
      if (SK != Z3_BV_SORT)
        throw std::runtime_error("unexpected numeral sort");
      unsigned W = Z3_get_bv_sort_size(Ctx, S);
      const char *Num = Z3_get_numeral_string(Ctx, E);
      if (!Num)
        throw std::runtime_error("numeral string missing");
      IntegerType *IT = IntegerType::get(F.Ctx, W);
      APInt V(W, StringRef(Num), 10);
      return F.intConst(V, IT);
    }

    if (AK != Z3_APP_AST)
      throw std::runtime_error("unsupported ast kind");

    z3::func_decl D = E.decl();
    Z3_decl_kind DK = static_cast<Z3_decl_kind>(Z3_get_decl_kind(Ctx, D));
    std::string Name = D.name().str();

    if (E.num_args() == 0 && DK == Z3_OP_UNINTERPRETED) {
      auto It = NameToExpr.find(Name);
      if (It != NameToExpr.end())
        return It->second;
      throw std::runtime_error("unknown const");
    }

    auto arg = [&](unsigned i) { return fromZ3(E.arg(i)); };

    switch (DK) {
    case Z3_OP_NOT:
      return F.not_(arg(0));
    case Z3_OP_AND: {
      std::vector<ExprRef> Kids;
      Kids.reserve(E.num_args());
      for (unsigned i = 0; i < E.num_args(); ++i)
        Kids.push_back(arg(i));
      return F.and_(Kids);
    }
    case Z3_OP_OR: {
      std::vector<ExprRef> Kids;
      Kids.reserve(E.num_args());
      for (unsigned i = 0; i < E.num_args(); ++i)
        Kids.push_back(arg(i));
      return F.or_(Kids);
    }
    case Z3_OP_EQ: {
      ExprRef A = arg(0);
      ExprRef B = arg(1);
      if (A && A->Ty && A->Ty->isPointerTy())
        return ptrIcmp(CmpInst::ICMP_EQ, A, B);
      return F.icmp(CmpInst::ICMP_EQ, A, B);
    }
    case Z3_OP_DISTINCT: {
      if (E.num_args() != 2)
        throw std::runtime_error("distinct arity");
      ExprRef A = arg(0);
      ExprRef B = arg(1);
      if (A && A->Ty && A->Ty->isPointerTy())
        return ptrIcmp(CmpInst::ICMP_NE, A, B);
      return F.icmp(CmpInst::ICMP_NE, A, B);
    }
    case Z3_OP_BADD:
    case Z3_OP_ADD:
      return F.add(arg(0), arg(1));
    case Z3_OP_BSUB:
    case Z3_OP_SUB:
      return F.sub(arg(0), arg(1));
    case Z3_OP_BMUL:
    case Z3_OP_MUL:
      return F.mul(arg(0), arg(1));
    case Z3_OP_BAND:
      return F.band(arg(0), arg(1));
    case Z3_OP_BOR:
      return F.bor(arg(0), arg(1));
    case Z3_OP_BXOR:
      return F.bxor(arg(0), arg(1));
    case Z3_OP_BSHL:
      return F.shl(arg(0), arg(1));
    case Z3_OP_BLSHR:
      return F.lshr(arg(0), arg(1));
    case Z3_OP_BASHR:
      return F.ashr(arg(0), arg(1));
    case Z3_OP_BUDIV:
      return F.udiv(arg(0), arg(1));
    case Z3_OP_BSDIV:
      return F.sdiv(arg(0), arg(1));
    case Z3_OP_BUREM:
      return F.urem(arg(0), arg(1));
    case Z3_OP_BSREM:
      return F.srem(arg(0), arg(1));
    case Z3_OP_ULT:
      return F.icmp(CmpInst::ICMP_ULT, arg(0), arg(1));
    case Z3_OP_ULEQ:
      return F.icmp(CmpInst::ICMP_ULE, arg(0), arg(1));
    case Z3_OP_UGT:
      return F.icmp(CmpInst::ICMP_UGT, arg(0), arg(1));
    case Z3_OP_UGEQ:
      return F.icmp(CmpInst::ICMP_UGE, arg(0), arg(1));
    case Z3_OP_SLT:
      return F.icmp(CmpInst::ICMP_SLT, arg(0), arg(1));
    case Z3_OP_SLEQ:
      return F.icmp(CmpInst::ICMP_SLE, arg(0), arg(1));
    case Z3_OP_SGT:
      return F.icmp(CmpInst::ICMP_SGT, arg(0), arg(1));
    case Z3_OP_SGEQ:
      return F.icmp(CmpInst::ICMP_SGE, arg(0), arg(1));
    case Z3_OP_ITE: {
      if (E.num_args() != 3)
        throw std::runtime_error("ite arity");
      return F.select(arg(0), arg(1), arg(2));
    }
    case Z3_OP_UNINTERPRETED: {
      auto UI = UFInfo.find(Name);
      if (UI == UFInfo.end())
        throw std::runtime_error("unknown uf");
      const Z3UFInfo &Info = UI->second;
      if (Info.K == Z3UFInfo::Kind::Drf) {
        if (E.num_args() != 1)
          throw std::runtime_error("drf arity");
        return F.deref(arg(0), Info.RetTy);
      }
      if (Info.K == Z3UFInfo::Kind::Cast) {
        if (E.num_args() != 1)
          throw std::runtime_error("cast arity");
        return F.cast(Info.CastOp, Info.CastDstTy, arg(0));
      }
      if (Info.K == Z3UFInfo::Kind::Gep) {
        if (E.num_args() != (1 + static_cast<unsigned>(Info.GepNumIdx)))
          throw std::runtime_error("gep arity");
        ExprRef Base = arg(0);
        SmallVector<ExprRef, 8> Idxs;
        for (unsigned i = 1; i < E.num_args(); ++i)
          Idxs.push_back(arg(i));
        return F.gep(Info.GepSourceEltTy, Info.GepInBounds, Base, Idxs,
                     Info.GepResultTy);
      }
      throw std::runtime_error("unsupported uf kind");
    }
    default:
      break;
    }

    throw std::runtime_error("unsupported z3 op");
  }
};

} // namespace

ExprRef tryEliminateExistsByZ3QE(const ExprFactory &F, Module &M,
                                 const ExprRef &TrimCond) {
  if (!TrimCond)
    return nullptr;

  Z3IntSemanticsKind Sem = getZ3IntSemantics();
  Z3QESession S(F, M, Sem);
  try {
    z3::expr In = S.toZ3(TrimCond);

    z3::goal G(S.Ctx);
    G.add(In);

    z3::params P(S.Ctx);
    P.set("timeout", FDTrimQETTimeoutMs);

    z3::tactic QE(S.Ctx, "qe");
    // z3::tactic QE(S.Ctx, "qe2");
    QE = z3::with(QE, P);
    z3::apply_result R = QE(G);

    z3::expr Out = S.Ctx.bool_val(true);
    for (unsigned i = 0; i < R.size(); ++i)
      Out = Out && R[i].as_expr();

    Out = Out.simplify();
    if (Out.is_true())
      return F.boolConst(true);
    if (Out.is_false())
      return F.boolConst(false);

    if (Sem == Z3IntSemanticsKind::Math)
      return nullptr;

    return S.fromZ3(Out);
  } catch (...) {
    return nullptr;
  }
}

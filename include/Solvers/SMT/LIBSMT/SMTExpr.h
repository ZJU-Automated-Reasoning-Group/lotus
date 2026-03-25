#pragma once

#include "Solvers/SMT/LIBSMT/SMTObject.h"
#include "z3++.h"

#include <iostream>
#include <map>
#include <memory>

class SMTFactory;
class SMTExprVec;
class SMTExprComparator;

class SMTExpr : public SMTObject {
private:
  z3::expr Expr;

  SMTExpr(SMTFactory *F, z3::expr Z3Expr);

public:
  virtual ~SMTExpr() {}

  SMTExpr(SMTExpr const &E);

  SMTExpr &operator=(const SMTExpr &E);

#define DECLARE_UNARY_OPERATION_EXPR(X)                                        \
  friend SMTExpr operator X(SMTExpr const &);

  DECLARE_UNARY_OPERATION_EXPR(!)
  DECLARE_UNARY_OPERATION_EXPR(-)
  DECLARE_UNARY_OPERATION_EXPR(~)

#define DECLARE_BINARY_OPERATION_EXPR_EXPR(X)                                  \
  friend SMTExpr operator X(SMTExpr const &, SMTExpr const &);

  DECLARE_BINARY_OPERATION_EXPR_EXPR(||)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(&&)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(==)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(!=)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(+)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(-)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(*)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(/)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(>)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(<)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(>=)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(<=)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(|)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(&)
  DECLARE_BINARY_OPERATION_EXPR_EXPR(^)

#define DECLARE_BINARY_OPERATION_EXPR_T(X, T)                                  \
  friend SMTExpr operator X(SMTExpr const &, T);

  DECLARE_BINARY_OPERATION_EXPR_T(==, int)
  DECLARE_BINARY_OPERATION_EXPR_T(!=, int)
  DECLARE_BINARY_OPERATION_EXPR_T(+, int)
  DECLARE_BINARY_OPERATION_EXPR_T(-, int)
  DECLARE_BINARY_OPERATION_EXPR_T(*, int)
  DECLARE_BINARY_OPERATION_EXPR_T(/, int)
  DECLARE_BINARY_OPERATION_EXPR_T(>, int)
  DECLARE_BINARY_OPERATION_EXPR_T(<, int)
  DECLARE_BINARY_OPERATION_EXPR_T(>=, int)
  DECLARE_BINARY_OPERATION_EXPR_T(<=, int)
  DECLARE_BINARY_OPERATION_EXPR_T(&, int)
  DECLARE_BINARY_OPERATION_EXPR_T(|, int)
  DECLARE_BINARY_OPERATION_EXPR_T(^, int)
  DECLARE_BINARY_OPERATION_EXPR_T(||, bool)
  DECLARE_BINARY_OPERATION_EXPR_T(&&, bool)

#define DECLARE_BINARY_OPERATION_T_EXPR(X, T)                                  \
  friend SMTExpr operator X(T, SMTExpr const &);

  DECLARE_BINARY_OPERATION_T_EXPR(==, int)
  DECLARE_BINARY_OPERATION_T_EXPR(!=, int)
  DECLARE_BINARY_OPERATION_T_EXPR(+, int)
  DECLARE_BINARY_OPERATION_T_EXPR(-, int)
  DECLARE_BINARY_OPERATION_T_EXPR(*, int)
  DECLARE_BINARY_OPERATION_T_EXPR(/, int)
  DECLARE_BINARY_OPERATION_T_EXPR(>, int)
  DECLARE_BINARY_OPERATION_T_EXPR(<, int)
  DECLARE_BINARY_OPERATION_T_EXPR(>=, int)
  DECLARE_BINARY_OPERATION_T_EXPR(<=, int)
  DECLARE_BINARY_OPERATION_T_EXPR(&, int)
  DECLARE_BINARY_OPERATION_T_EXPR(|, int)
  DECLARE_BINARY_OPERATION_T_EXPR(^, int)
  DECLARE_BINARY_OPERATION_T_EXPR(||, bool)
  DECLARE_BINARY_OPERATION_T_EXPR(&&, bool)

  friend std::ostream &operator<<(std::ostream &, SMTExpr const &);

  bool SMTExprToStream(std::string &ExprStr);

  bool SMTExprToStream(std::ostream &Out);

  bool isLogicalEquivTo(SMTExpr const &E);

  bool isSameSort(SMTExpr const &E) const {
    return z3::eq(Expr.get_sort(), E.Expr.get_sort());
  }

  z3::sort getSort() { return Expr.get_sort(); }

  bool isArray() { return Expr.is_array(); }

  bool isBitVector() { return Expr.is_bv(); }

  bool isReal() { return Expr.is_real(); }

  bool isBool() { return Expr.is_bool(); }

  bool isBvArray() {
    if (Expr.is_array()) {
      return Expr.get_sort().array_range().is_bv();
    }
    return false;
  }

  bool isConst() const { return Expr.is_const(); }

  bool isApp() const { return Expr.is_app(); }

  bool isNumeral() const { return Expr.is_numeral(); }

  // Please verify isNumeral() before getNumeralUint64()
  uint64_t getNumeralUint64() {
    assert(isNumeral());
    return Expr.get_numeral_uint64();
  }

  bool isQuantifier() const { return Expr.is_quantifier(); }

  SMTExpr getQuantifierBody() const;

  bool isVar() const { return Expr.is_var(); }

  unsigned numArgs() const { return Expr.num_args(); }

  SMTExpr getArg(unsigned Index) const;

  unsigned getBitVecSize() { return Expr.get_sort().bv_size(); }

  bool isLogicAnd() const { return Expr.decl().decl_kind() == Z3_OP_AND; }

  bool isLogicOr() const { return Expr.decl().decl_kind() == Z3_OP_OR; }

  bool isLogicNot() const { return Expr.decl().decl_kind() == Z3_OP_NOT; }

  bool isTrue() const { return Expr.decl().decl_kind() == Z3_OP_TRUE; }

  bool isFalse() const { return Expr.decl().decl_kind() == Z3_OP_FALSE; }

  bool equals(const SMTExpr &e) const { return z3::eq(Expr, e.Expr); }

  std::string getSymbol() const { return Z3_ast_to_string(Expr.ctx(), Expr); }

  Z3_symbol getZ3Symbol() const { return Expr.decl().name(); }

  z3::expr &getZ3Expr() { return Expr; }

  unsigned getAstId() const { return Z3_get_ast_id(Expr.ctx(), Expr); }

  SMTExpr substitute(SMTExprVec &From, SMTExprVec &To);

  /*==-- basic operations --==*/

  SMTExpr bv12bool();

  SMTExpr bool2bv1();

  SMTExpr real2int();

  SMTExpr int2real();

  SMTExpr int2bv(unsigned Sz);

  SMTExpr bv2int(bool IsSigned);

#define DECLARE_BASIC_OPERATION(X) SMTExpr basic_##X(SMTExpr &Expr);

  DECLARE_BASIC_OPERATION(add)
  DECLARE_BASIC_OPERATION(sub)
  DECLARE_BASIC_OPERATION(mul)
  DECLARE_BASIC_OPERATION(udiv)
  DECLARE_BASIC_OPERATION(sdiv)
  DECLARE_BASIC_OPERATION(urem)
  DECLARE_BASIC_OPERATION(srem)
  DECLARE_BASIC_OPERATION(shl)
  DECLARE_BASIC_OPERATION(ashr)
  DECLARE_BASIC_OPERATION(lshr)
  DECLARE_BASIC_OPERATION(and)
  DECLARE_BASIC_OPERATION(or)
  DECLARE_BASIC_OPERATION(xor)
  DECLARE_BASIC_OPERATION(concat)

  DECLARE_BASIC_OPERATION(eq)
  DECLARE_BASIC_OPERATION(ne)
  DECLARE_BASIC_OPERATION(ugt)
  DECLARE_BASIC_OPERATION(uge)
  DECLARE_BASIC_OPERATION(sgt)
  DECLARE_BASIC_OPERATION(sge)
  DECLARE_BASIC_OPERATION(ult)
  DECLARE_BASIC_OPERATION(ule)
  DECLARE_BASIC_OPERATION(slt)
  DECLARE_BASIC_OPERATION(sle)

  SMTExpr basic_extract(unsigned High, unsigned Low);

  SMTExpr basic_zext(unsigned Sz);

  SMTExpr basic_sext(unsigned Sz);

  SMTExpr basic_ite(SMTExpr &TBValue, SMTExpr &FBValue);

  /*==-- array operations --==*/
  SMTExpr array_elmt(unsigned ElmtNum, unsigned Index);

#define DECLARE_ARRAY_OPERATION(X)                                             \
  SMTExpr array_##X(SMTExpr &BvAsArray, unsigned ElmtNum);

  DECLARE_ARRAY_OPERATION(sgt)
  DECLARE_ARRAY_OPERATION(sge)
  DECLARE_ARRAY_OPERATION(ugt)
  DECLARE_ARRAY_OPERATION(uge)
  DECLARE_ARRAY_OPERATION(slt)
  DECLARE_ARRAY_OPERATION(sle)
  DECLARE_ARRAY_OPERATION(ule)
  DECLARE_ARRAY_OPERATION(ult)
  DECLARE_ARRAY_OPERATION(eq)
  DECLARE_ARRAY_OPERATION(ne)

  DECLARE_ARRAY_OPERATION(add)
  DECLARE_ARRAY_OPERATION(sub)
  DECLARE_ARRAY_OPERATION(mul)
  DECLARE_ARRAY_OPERATION(udiv)
  DECLARE_ARRAY_OPERATION(sdiv)
  DECLARE_ARRAY_OPERATION(urem)
  DECLARE_ARRAY_OPERATION(srem)
  DECLARE_ARRAY_OPERATION(shl)
  DECLARE_ARRAY_OPERATION(ashr)
  DECLARE_ARRAY_OPERATION(lshr)
  DECLARE_ARRAY_OPERATION(and)
  DECLARE_ARRAY_OPERATION(or)
  DECLARE_ARRAY_OPERATION(xor)

  SMTExpr array_ite(SMTExpr &TBValue, SMTExpr &FBValue, unsigned ElmtNum);

  // Sz: size to extend
  SMTExpr array_sext(unsigned Sz, unsigned ElmtNum);

  // Sz: size to extend
  SMTExpr array_zext(unsigned Sz, unsigned ElmtNum);

  // Sz: size to trunc
  SMTExpr array_trunc(unsigned Sz, unsigned ElmtNum);

  /*==-- simplifications --==*/
  SMTExpr localSimplify();

  SMTExpr ctxSimplify();

  SMTExpr doConstantPropagation();

  SMTExpr elimValidOrUnsatExpr();

  /******************************
   * Existential quantifier elimination facilities.

   "qe2" is based on "Playing with quantified satisfaction, LPAR 15" that uses
   "model based projection" proposed in CAV 14.

   We use the name "forget" because this procedure(or its variant) is often
   called the "forget operator" both by the abstract interpretation community
   and the artificial intelligence community.

   Given P(x, y, z), What is the meaning of QE(Exists x,y. P)?
   1. The strongest consequence of P that contains only z.
   2. The strongest necessary condition on z to make P satisfiable
   3. The best symbolic abstraction of P that only uses z.
   4. The strongest post-condition ...
   5. The ...

   ********************************/
  // the expression is already extentially quantified
  std::pair<bool, SMTExpr> forgetVars(unsigned Timeout = 2000);

  // eliminate a single Var
  std::pair<bool, SMTExpr> forgetVar(SMTExpr &Var, unsigned Timeout = 2000);

  std::pair<bool, SMTExpr> forgetVars(SMTExprVec &Vars,
                                      unsigned Timeout = 2000);

  /********************
   *  Generalized consequence finding or symbolic abstraction
   *
   *  Given P(x,y,z), find its strongest consequence/best abstraction in an
   abstract domain.
   *
   *  TODO:
   *  1. more domains, and automatic generation of transformers
   *  2. how to interact with LLVM values and even LLVM passes?

   **********************/

  // get the Interval information about Var
  // SMTExpr abstractInterval(SMTExpr& Var, int Timeout=2000);
  std::pair<int, int> abstractInterval(SMTExpr &Var, int Timeout = 5000);

  void abstractIntervalAsExprvec(SMTExpr &Var, SMTExprVec &Evec,
                                 int Timeout = 5000);

  SMTExpr dilligSimplify();

  // apply lightweight simplification at a time, including a set of
  // simple Boolean- and theory- level rewritings and constant propagation
  SMTExpr lightweightSimpifyAllInOne();

  // get the bit-vector variables
  bool getExprVars(SMTExprVec &Vars);

  unsigned size(std::map<SMTExpr, unsigned, SMTExprComparator> &);

  friend class SMTFactory;
  friend class SMTSolver;
  friend class SMTExprVec;
  friend class SMTExprComparator;
  friend class SMTModel;

private:
  SMTExpr dilligSimplify(SMTExpr N, z3::solver &Solver4Sim, z3::context &Ctx);
};

// This can be used as the comparator of a stl container,
// e.g. SMTExprComparator comparator; map<SMTExpr, value, comparator> ...;
class SMTExprComparator {
public:
  bool operator()(const SMTExpr &X, const SMTExpr &Y) const {
    unsigned XId = Z3_get_ast_id(X.Expr.ctx(), X.Expr);
    unsigned YId = Z3_get_ast_id(Y.Expr.ctx(), Y.Expr);
    return XId < YId;
  }
};

/**
 * NOTE: when SMTExprVec.empty(), the copy constructor
 * and = operator have the copy semantics. Otherwise,
 * they have move semantics.
 */
class SMTExprVec : public SMTObject {
private:
  std::shared_ptr<z3::expr_vector> ExprVec;

  SMTExprVec(SMTFactory *F, std::shared_ptr<z3::expr_vector> Vec);

public:
  ~SMTExprVec() {}

  SMTExprVec(const SMTExprVec &Vec);

  SMTExprVec &operator=(const SMTExprVec &Vec);

  unsigned size() const;

  unsigned constraintSize() const;

  // If Enforce is set, all exprs will be added to the vector,
  // otherwise, "true" will be filtered
  void push_back(SMTExpr Expr, bool Enforce = false);

  SMTExpr operator[](unsigned I) const;

  bool empty() const;

  SMTExprVec copy();

  /// *this = *this && v2
  void mergeWithAnd(const SMTExprVec &Vec);

  static SMTExprVec merge(SMTExprVec Vec1, SMTExprVec Vec2);

  /// *this = *this || v2
  void mergeWithOr(const SMTExprVec &Vec);

  /// From the vector create one expr to represent the AND result.
  /// Create  bool expr to represent this vector
  SMTExpr toAndExpr() const;

  SMTExpr toOrExpr() const;

  // Set difference of variables in *this and the passed Vars
  // note that we assume *this and Vars consist of purely variables.
  SMTExprVec setDifference(const SMTExprVec &Vars);

  friend class SMTFactory;
  friend class SMTSolver;
  friend class SMTExpr;

  friend std::ostream &operator<<(std::ostream &Out, SMTExprVec Vec);

  bool SMTExprVecToStream(std::string &ExprStr);

  bool SMTExprVecToStream(std::ostream &Out);
};

//===- Z3Expr.h -- Z3 Expression wrapper for SABER ------------------===//
//
// Simplified Z3 expression interface for path condition tracking.
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef USE_Z3
#include <z3++.h>
#endif

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lotus {
namespace analysis {

class Z3Expr {
public:
#ifdef USE_Z3
  using Z3Context = z3::context;
  using Z3ExprImpl = z3::expr;
  static Z3Context &context();
#else
  struct DummyExpr {
    uint32_t id_;
    DummyExpr() : id_(0) {}
    DummyExpr(uint32_t id) : id_(id) {}
    uint32_t id() const { return id_; }
  };
  using Z3ExprImpl = DummyExpr;
#endif

private:
  Z3ExprImpl expr_;
  static uint32_t nextId_;

public:
  Z3Expr()
#ifdef USE_Z3
      : expr_(context().bool_val(false)){}
#else
      : expr_() {
    expr_ = DummyExpr(nextId_++);
  }
#endif

#ifdef USE_Z3
        Z3Expr(const z3::expr &e)
      : expr_(e) {
  }
  Z3Expr(z3::expr &&e) : expr_(std::move(e)) {}
#endif

  uint32_t id() const {
#ifdef USE_Z3
    return expr_.id();
#else
    return expr_.id();
#endif
  }

  static Z3Expr getTrueCond() {
#ifdef USE_Z3
    return Z3Expr(context().bool_val(true));
#else
    return Z3Expr();
#endif
  }

  static Z3Expr getFalseCond() {
#ifdef USE_Z3
    return Z3Expr(context().bool_val(false));
#else
    Z3Expr e;
    return e;
#endif
  }

  static Z3Expr nullExpr() {
#ifdef USE_Z3
    return Z3Expr(context().bool_const("_null_"));
#else
    Z3Expr e;
    return e;
#endif
  }

  static Z3Expr AND(const Z3Expr &lhs, const Z3Expr &rhs) {
#ifdef USE_Z3
    return Z3Expr(lhs.expr_ && rhs.expr_);
#else
    return Z3Expr();
#endif
  }

  static Z3Expr OR(const Z3Expr &lhs, const Z3Expr &rhs) {
#ifdef USE_Z3
    return Z3Expr(lhs.expr_ || rhs.expr_);
#else
    return Z3Expr();
#endif
  }

  static Z3Expr NEG(const Z3Expr &expr) {
#ifdef USE_Z3
    return Z3Expr(!expr.expr_);
#else
    return Z3Expr();
#endif
  }

  static std::string dumpStr(const Z3Expr &expr) {
#ifdef USE_Z3
    return expr.expr_.to_string();
#else
    return "expr_" + std::to_string(expr.id());
#endif
  }

#ifdef USE_Z3
  const z3::expr &getExpr() const { return expr_; }
  z3::expr &getExpr() { return expr_; }
#endif

  bool operator==(const Z3Expr &other) const { return id() == other.id(); }
  bool operator!=(const Z3Expr &other) const { return id() != other.id(); }
  bool operator<(const Z3Expr &other) const { return id() < other.id(); }
};

} // namespace analysis
} // namespace lotus

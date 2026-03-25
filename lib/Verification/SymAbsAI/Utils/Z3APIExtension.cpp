#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <functional>
#include <iostream>
#include <set>

using namespace z3;

namespace z3_ext {

expr shl(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvshl(a.ctx(), a, b));
}

expr lshr(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvlshr(a.ctx(), a, b));
}

expr ashr(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvashr(a.ctx(), a, b));
}

expr extract(unsigned int high, unsigned int low, expr const &b) {
  // extracts from high downto lo (both including!)
  return to_expr(b.ctx(), Z3_mk_extract(b.ctx(), high, low, b));
}

expr concat(expr const &a, expr const &b) {
  return to_expr(a.ctx(), Z3_mk_concat(a.ctx(), a, b));
}

expr zext(unsigned int num, expr const &b) {
  // extends to size |b|+num
  return to_expr(b.ctx(), Z3_mk_zero_ext(b.ctx(), num, b));
}

expr sext(unsigned int num, expr const &b) {
  // extends to size |b|+num
  return to_expr(b.ctx(), Z3_mk_sign_ext(b.ctx(), num, b));
}

expr urem(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvurem(a.ctx(), a, b));
}

expr srem(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvsrem(a.ctx(), a, b));
}

expr add_nof(expr const &a, expr const &b, bool isSigned) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvadd_no_overflow(a.ctx(), a, b, isSigned));
}

expr add_nuf(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvadd_no_underflow(a.ctx(), a, b));
}

expr sub_nuf(expr const &a, expr const &b, bool isSigned) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvsub_no_underflow(a.ctx(), a, b, isSigned));
}

expr sub_nof(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvsub_no_overflow(a.ctx(), a, b));
}

expr mul_nof(expr const &a, expr const &b, bool isSigned) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvmul_no_overflow(a.ctx(), a, b, isSigned));
}

expr mul_nuf(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvmul_no_underflow(a.ctx(), a, b));
}

expr sdiv_nof(expr const &a, expr const &b) {
  check_context(a, b);
  return to_expr(a.ctx(), Z3_mk_bvsdiv_no_overflow(a.ctx(), a, b));
}
} // namespace z3_ext

uint64_t expr_to_uint(const z3::expr &e) {
  uint64_t result;
  bool succ = Z3_get_numeral_uint64(e.ctx(), e, &result);

  (void)succ;
  assert(succ && "unsupported conversion from numeral");

  return result;
}

int64_t expr_to_int(const z3::expr &e) {
  uint64_t res;
  bool succ = Z3_get_numeral_uint64(e.ctx(), e, &res);
  // this cast is necessary because the respective z3 function for signed
  // ints does not accept negative values (as they are interpreted as too
  // large values that do not fit into a signed int)
  int64_t result = (int64_t)res;
  if (!succ)
    std::cerr << e << '\n';
  assert(succ && "unsupported conversion from numeral");
  unsigned int size = e.get_sort().bv_size();
  if (size < 64 && (result & (1L << (size - 1L)))) {
    // need to sign extend
    result = ~((1L << size) - 1L) | result;
  }
  return result;
}

bool expr_to_bool(const z3::expr &e) {
  assert(e.is_const());
  assert(e.is_bool());

  return z3::eq(e, e.ctx().bool_val(true));
}

bool model_defines(const z3::model &model, const z3::symbol &sym) {
  for (unsigned i = 0; i < model.num_consts(); i++) {
    if (model.get_const_decl(i).name() == sym)
      return true;
  }
  return false;
}

bool is_unsat(z3::expr e) {
  z3::solver solver(e.ctx());
  solver.add(e);
  return solver.check() == z3::unsat;
}

std::vector<expr> expr_constants(const expr &e) {
  auto &ctx = e.ctx();

  auto compare_func = [](const expr &a, const expr &b) {
    Z3_symbol sym_a = a.decl().name();
    Z3_symbol sym_b = b.decl().name();
    return sym_a < sym_b;
  };
  std::set<expr, decltype(compare_func)> syms(compare_func);

  std::function<void(const expr &)> recur = [&recur, &syms,
                                             &ctx](const expr &e) {
    assert(Z3_is_app(ctx, e));
    auto app = Z3_to_app(ctx, e);
    unsigned n_args = Z3_get_app_num_args(ctx, app);

    auto fdecl = Z3_get_app_decl(ctx, app);
    if (n_args == 0 && Z3_get_decl_kind(ctx, fdecl) == Z3_OP_UNINTERPRETED)
      syms.insert(e);

    for (unsigned i = 0; i < n_args; ++i) {
      z3::expr arg(ctx, Z3_get_app_arg(ctx, app, i));
      recur(arg);
    }
  };
  recur(e);

  return std::vector<expr>(syms.begin(), syms.end());
}

z3::expr adjustBitwidth(z3::expr op, unsigned int to_size) {
  assert(op.is_bv() && "Only Bitvectors supported here!");
  unsigned int from_size = op.get_sort().bv_size();
  if (to_size < from_size) // truncate
    return z3_ext::extract(to_size - 1, 0, op);
  else if (to_size > from_size) // zero extend
    return z3_ext::zext(to_size - from_size, op);
  else
    return op;
}

z3::expr makeConstantInt(z3::context *ctx, const llvm::ConstantInt *value) {
  int bits = value->getType()->getIntegerBitWidth();
  assert(bits <= 64 && "TODO: support bigger ints");

  uint64_t val = llvm::dyn_cast<llvm::ConstantInt>(value)->getLimitedValue();
  z3::expr cexpr = ctx->bv_val((uint64_t)val, bits);
  return cexpr;
}

void makePairSort(z3::context &ctx, z3::sort &dest_sort,
                  z3::func_decl &dest_get_a, z3::func_decl &dest_get_b,
                  z3::func_decl &dest_constr, const char *src_get_a_name,
                  z3::sort src_get_a_sort, const char *src_get_b_name,
                  z3::sort src_get_b_sort, const char *src_constr_name) {
  Z3_symbol mk_tuple_name = Z3_mk_string_symbol(ctx, src_constr_name);
  Z3_symbol proj_names[2];
  Z3_sort proj_sorts[2];
  Z3_func_decl constr_decl;
  Z3_func_decl proj_decls[2];

  proj_names[0] = Z3_mk_string_symbol(ctx, src_get_a_name);
  proj_names[1] = Z3_mk_string_symbol(ctx, src_get_b_name);

  proj_sorts[0] = src_get_a_sort;
  proj_sorts[1] = src_get_b_sort;

  Z3_sort res = Z3_mk_tuple_sort(ctx, mk_tuple_name, 2, proj_names, proj_sorts,
                                 &constr_decl, proj_decls);

  dest_sort = z3::to_sort(ctx, res);
  dest_get_a = z3::to_func_decl(ctx, proj_decls[0]);
  dest_get_b = z3::to_func_decl(ctx, proj_decls[1]);
  dest_constr = z3::to_func_decl(ctx, constr_decl);
}

#pragma once
/**
 * @file Z3APIExtension.h
 * @Brief This class is an extension of the z3 C++ API which omits in a
 * completely incomprehensive way important functions for handling bitvectors.
 * The functions here use low level functions to generate the expressions in
 * a way that is similar to how the other operations are implemented in the
 * C++ API.
 */

#include <vector>

#include <llvm/IR/Constants.h>
#include <z3++.h>

namespace z3_ext {

z3::expr shl(z3::expr const &a, z3::expr const &b);

z3::expr lshr(z3::expr const &a, z3::expr const &b);

z3::expr ashr(z3::expr const &a, z3::expr const &b);

// extracts from high downto lo (both including!)
z3::expr extract(unsigned int high, unsigned int low, z3::expr const &b);

// concatenates bitvectors
z3::expr concat(z3::expr const &a, z3::expr const &b);

// extends to size |b|+num
z3::expr zext(unsigned int num, z3::expr const &b);

// extends to size |b|+num
z3::expr sext(unsigned int num, z3::expr const &b);

z3::expr urem(z3::expr const &a, z3::expr const &b);

z3::expr srem(z3::expr const &a, z3::expr const &b);

// creates a predicate which states that no overflow occurred
z3::expr add_nof(z3::expr const &a, z3::expr const &b, bool isSigned);

// creates a predicate which states that no underflow occurred
z3::expr add_nuf(z3::expr const &a, z3::expr const &b);

// creates a predicate which states that no underflow occurred
z3::expr sub_nuf(z3::expr const &a, z3::expr const &b, bool isSigned);

// creates a predicate which states that no overflow occurred
z3::expr sub_nof(z3::expr const &a, z3::expr const &b);

// creates a predicate which states that no overflow occurred
z3::expr mul_nof(z3::expr const &a, z3::expr const &b, bool isSigned);

// creates a predicate which states that no underflow occurred
z3::expr mul_nuf(z3::expr const &a, z3::expr const &b);

// creates a predicate which states that no overflow occurred
z3::expr sdiv_nof(z3::expr const &a, z3::expr const &b);
} // namespace z3_ext

/**
 * Convertes a Z3 expression to an unsigned integer.
 *
 * The expression must be a constant bitvector of 64 bits or less.
 */
uint64_t expr_to_uint(const z3::expr &e);

/**
 * Convertes a Z3 expression to a signed integer.
 *
 * The expression must be a constant bitvector of 64 bits or less.
 */
int64_t expr_to_int(const z3::expr &e);

bool expr_to_bool(const z3::expr &e);

/**
 * Determines whether a model contains a definition for given symbol.
 *
 * Models returned by Z3 sometimes don't define all constant values. In such
 * case, calling model::eval returns a variable which is problematic to handle.
 */
bool model_defines(const z3::model &model, const z3::symbol &sym);

/**
 * Returns true if given formula is unsatisfiable.
 *
 * Intended for quick checks in assertions.
 */
bool is_unsat(z3::expr e);

/**
 * Returns all the constants (unterpreted symbols of arity 0) in the
 * given expression.
 */
std::vector<z3::expr> expr_constants(const z3::expr &e);

/**
 * Truncate, zero extend or nop-cast `op` to `to_size`.
 *
 */
z3::expr adjustBitwidth(z3::expr op, unsigned int to_size);

/**
 * Returns a z3 bit vector expression that represents the given value.
 */
z3::expr makeConstantInt(z3::context *ctx, const llvm::ConstantInt *value);

/**
 * Creates a Z3 tuple sort with two components. `dest_*` arguments are filled
 * with constructed z3 entities and `src_*` arguments are used to construct
 * these.
 */
void makePairSort(z3::context &ctx, z3::sort &dest_sort,
                  z3::func_decl &dest_get_a, z3::func_decl &dest_get_b,
                  z3::func_decl &dest_constr, const char *src_get_a_name,
                  z3::sort src_get_a_sort, const char *src_get_b_name,
                  z3::sort src_get_b_sort, const char *src_constr_name);

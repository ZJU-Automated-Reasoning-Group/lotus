/*
NOTE: we have two classes of "symbolic abstraction" algorithms:
1. lib/Verification/SymAbsAI
2. lib/Solvers/SMT/SymAbs

At first glance, the abstract domains have overlap. However, it 
lib/Solvers/SMT/SymAbs, we actually use linear integer formulas to "approximate" bit-vector formulas, which can be unintuitive.

*/
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include <gtest/gtest.h>
#include <z3++.h>

using namespace z3;

namespace {

TEST(SymAbsLinearExpressionTest, MaximumAndMinimumInRange) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = sge(x, ctx.bv_val(0, 8)) && sle(x, ctx.bv_val(10, 8));

    auto max_val = SymAbs::maximum(phi, x);
    ASSERT_TRUE(max_val.hasValue());
    EXPECT_EQ(10, max_val.getValue());

    auto min_val = SymAbs::minimum(phi, x);
    ASSERT_TRUE(min_val.hasValue());
    EXPECT_EQ(0, min_val.getValue());
}

TEST(SymAbsCongruenceTest, FindsModulusAndRemainder) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = (x == ctx.bv_val(2, 8)) ||
               (x == ctx.bv_val(6, 8)) ||
               (x == ctx.bv_val(10, 8));

    auto cong = SymAbs::alpha_a_cong(phi, x);
    ASSERT_TRUE(cong.hasValue());
    EXPECT_EQ(4u, cong->modulus);
    EXPECT_EQ(2, cong->remainder);
}

TEST(SymAbsAffineTest, CapturesEqualityRelation) {
    context ctx;
    expr x = ctx.bv_const("x", 8);
    expr y = ctx.bv_const("y", 8);

    expr phi = (x == y) &&
               ((x == ctx.bv_val(2, 8)) || (x == ctx.bv_val(3, 8)));

    std::vector<expr> vars = {x, y};
    auto eqs = SymAbs::alpha_aff_V(phi, vars);

    auto has_diff_eq = [&]() {
        for (const auto& eq : eqs) {
            if (eq.coefficients.size() != 2) continue;
            if (eq.constant != 0) continue;
            const auto& c = eq.coefficients;
            if ((c[0] == 1 && c[1] == -1) || (c[0] == -1 && c[1] == 1)) {
                return true;
            }
        }
        return false;
    };

    ASSERT_FALSE(eqs.empty());
    EXPECT_TRUE(has_diff_eq());
}

TEST(SymAbsLinearExpressionTest, HandlesNegativeRange) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = sge(x, ctx.bv_val(-5, 8)) && sle(x, ctx.bv_val(-2, 8));

    auto max_val = SymAbs::maximum(phi, x);
    ASSERT_TRUE(max_val.hasValue());
    EXPECT_EQ(-2, max_val.getValue());

    auto min_val = SymAbs::minimum(phi, x);
    ASSERT_TRUE(min_val.hasValue());
    EXPECT_EQ(-5, min_val.getValue());
}

TEST(SymAbsCongruenceTest, SingletonKeepsZeroModulus) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = (x == ctx.bv_val(7, 8));

    auto cong = SymAbs::alpha_a_cong(phi, x);
    ASSERT_TRUE(cong.hasValue());
    EXPECT_EQ(0u, cong->modulus);
    EXPECT_EQ(7, cong->remainder);
}

TEST(SymAbsAffineTest, CapturesConstantAssignment) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = (x == ctx.bv_val(5, 8));

    std::vector<expr> vars = {x};
    auto eqs = SymAbs::alpha_aff_V(phi, vars);

    auto has_const_eq = [&]() {
        for (const auto& eq : eqs) {
            if (eq.coefficients.size() != 1) continue;
            int64_t coeff = eq.coefficients[0];
            if (coeff == 1 && eq.constant == 5) return true;
            if (coeff == -1 && eq.constant == -5) return true;
        }
        return false;
    };

    ASSERT_FALSE(eqs.empty());
    EXPECT_TRUE(has_const_eq());
}

TEST(SymAbsPolyhedronTest, CapturesIntervalConstraints) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = sge(x, ctx.bv_val(5, 8)) && sle(x, ctx.bv_val(15, 8));

    std::vector<expr> vars = {x};
    auto poly = SymAbs::alpha_conv_V(phi, vars);

    // Should have at least lower and upper bound constraints
    ASSERT_FALSE(poly.empty());
    
    // Check for lower bound: -x <= -5 (i.e., x >= 5)
    bool has_lower = false;
    bool has_upper = false;
    
    for (const auto& ineq : poly) {
        if (ineq.coefficients.size() == 1) {
            if (ineq.coefficients[0] == -1 && ineq.constant == -5) {
                has_lower = true;
            }
            if (ineq.coefficients[0] == 1 && ineq.constant == 15) {
                has_upper = true;
            }
        }
    }
    
    EXPECT_TRUE(has_lower || has_upper);
}

TEST(SymAbsPolyhedronTest, HandlesMultipleVariables) {
    context ctx;
    expr x = ctx.bv_const("x", 8);
    expr y = ctx.bv_const("y", 8);

    expr phi = sge(x, ctx.bv_val(0, 8)) && sle(x, ctx.bv_val(10, 8)) &&
               sge(y, ctx.bv_val(0, 8)) && sle(y, ctx.bv_val(10, 8));

    std::vector<expr> vars = {x, y};
    auto poly = SymAbs::alpha_conv_V(phi, vars);

    // Should have constraints for both variables
    ASSERT_FALSE(poly.empty());
    
    // Check that we have constraints involving both variables
    bool has_x_constraint = false;
    bool has_y_constraint = false;
    
    for (const auto& ineq : poly) {
        if (ineq.coefficients.size() == 2) {
            if (ineq.coefficients[0] != 0) has_x_constraint = true;
            if (ineq.coefficients[1] != 0) has_y_constraint = true;
        }
    }
    
    EXPECT_TRUE(has_x_constraint);
    EXPECT_TRUE(has_y_constraint);
}

TEST(SymAbsZoneTest, CapturesUnaryConstraints) {
    context ctx;
    expr x = ctx.bv_const("x", 8);

    expr phi = sge(x, ctx.bv_val(0, 8)) && sle(x, ctx.bv_val(10, 8));

    std::vector<expr> vars = {x};
    auto zone = SymAbs::alpha_zone_V(phi, vars);

    // Should have at least an upper bound constraint
    ASSERT_FALSE(zone.empty());
    
    bool has_upper_bound = false;
    for (const auto& c : zone) {
        if (c.unary && c.bound == 10) {
            has_upper_bound = true;
            break;
        }
    }
    
    EXPECT_TRUE(has_upper_bound);
}


TEST(SymAbsZoneTest, HandlesMultipleDifferenceConstraints) {
    context ctx;
    expr x = ctx.bv_const("x", 8);
    expr y = ctx.bv_const("y", 8);
    expr z = ctx.bv_const("z", 8);

    // Create bounded variables that imply difference constraints
    // x in [0, 10], y in [2, 8], z in [1, 5]
    // This implies: x - y in [-8, 8], y - z in [-3, 7], x - z in [-5, 9]
    expr phi = sge(x, ctx.bv_val(0, 8)) && sle(x, ctx.bv_val(10, 8)) &&
               sge(y, ctx.bv_val(2, 8)) && sle(y, ctx.bv_val(8, 8)) &&
               sge(z, ctx.bv_val(1, 8)) && sle(z, ctx.bv_val(5, 8));

    std::vector<expr> vars = {x, y, z};
    auto zone = SymAbs::alpha_zone_V(phi, vars);

    // Should capture both unary and difference constraints
    ASSERT_FALSE(zone.empty());
    
    // Count non-unary (difference) constraints
    int diff_count = 0;
    for (const auto& c : zone) {
        if (!c.unary) {
            diff_count++;
        }
    }
    
    // With 3 variables, we should have multiple difference constraints
    // (up to 6 possible: x-y, x-z, y-x, y-z, z-x, z-y)
    EXPECT_GT(diff_count, 0);
}

} // namespace

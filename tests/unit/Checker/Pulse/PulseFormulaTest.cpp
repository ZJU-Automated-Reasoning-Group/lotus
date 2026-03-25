#include "Checker/Pulse/Core/PulseFormula.h"
#include <gtest/gtest.h>

namespace pulse {

TEST(PulseFormulaTest, DisequalityContradictsEquality) {
    PulseFormula f;
    AbstractValue a(nullptr, 1);
    AbstractValue b(nullptr, 2);

    EXPECT_TRUE(f.addDisequality(a, b));
    EXPECT_FALSE(f.addEquality(a, b));
}

TEST(PulseFormulaTest, EqualityContradictsDisequality) {
    PulseFormula f;
    AbstractValue a(nullptr, 1);
    AbstractValue b(nullptr, 2);

    EXPECT_TRUE(f.addEquality(a, b));
    EXPECT_FALSE(f.addDisequality(a, b));
}

TEST(PulseFormulaTest, DisequalityPreservedThroughUnion) {
    PulseFormula f;
    AbstractValue a(nullptr, 1);
    AbstractValue b(nullptr, 2);
    AbstractValue c(nullptr, 3);

    EXPECT_TRUE(f.addDisequality(a, b));
    EXPECT_TRUE(f.addEquality(b, c));
    EXPECT_TRUE(f.areDisequal(a, c));
}

TEST(PulseFormulaTest, InconsistentWhenUnionViolatesDisequality) {
    PulseFormula f;
    AbstractValue a(nullptr, 1);
    AbstractValue b(nullptr, 2);
    AbstractValue c(nullptr, 3);

    EXPECT_TRUE(f.addDisequality(a, b));
    EXPECT_TRUE(f.addEquality(b, c));
    EXPECT_FALSE(f.addEquality(a, c));
}

} // namespace pulse

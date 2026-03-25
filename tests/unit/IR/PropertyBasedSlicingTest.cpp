#include "IR/PDG/Analysis/PropertyBasedSlicing.h"

#include <gtest/gtest.h>
#include <sstream>

using namespace pdg;

TEST(PropertySpecTest, ParseUnreachCall) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "CHECK( init(main()), LTL(G ! call(reach_error())) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::UnreachCall);
  EXPECT_EQ(spec.rules()[0].target, "reach_error");
  EXPECT_EQ(spec.rules()[0].type, PropertyType::CHECK);
}

TEST(PropertySpecTest, ParseMemSafety) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "CHECK( init(main()), LTL(G valid-deref) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::MemSafety);
  EXPECT_EQ(spec.rules()[0].type, PropertyType::CHECK);
}

TEST(PropertySpecTest, ParseNoOverflow) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "CHECK( init(main()), LTL(G ! overflow) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::NoOverflow);
  EXPECT_TRUE(spec.rules()[0].negated);
}

TEST(PropertySpecTest, ParseTermination) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "CHECK( init(main()), LTL(F end) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::Termination);
}

TEST(PropertySpecTest, ParseCoverage) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "COVER( init(main()), FQL(COVER EDGES(@DECISIONEDGE)) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::CoverageBranches);
  EXPECT_EQ(spec.rules()[0].type, PropertyType::COVER);
}

TEST(PropertySpecTest, ParseCoverageErrorCall) {
  PropertySpec spec;
  std::string error;

  std::string content =
      "COVER( init(main()), FQL(COVER EDGES(@CALL(reach_error))) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::CoverageErrorCall);
  EXPECT_EQ(spec.rules()[0].target, "reach_error");
  EXPECT_EQ(spec.rules()[0].type, PropertyType::COVER);
}

TEST(PropertySpecTest, ParseQuotedCallTarget) {
  PropertySpec spec;
  std::string error;

  std::string content =
      "CHECK( init(main()), LTL(G ! call(\"__VERIFIER_error\")) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::UnreachCall);
  EXPECT_EQ(spec.rules()[0].target, "__VERIFIER_error");
}

TEST(PropertySpecTest, ParseCallTargetWithoutParens) {
  PropertySpec spec;
  std::string error;

  std::string content =
      "CHECK( init(main()), LTL(G ! call(reach_error)) )\n";
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::UnreachCall);
  EXPECT_EQ(spec.rules()[0].target, "reach_error");
}

TEST(PropertySpecTest, ParseAssertionsKeyword) {
  PropertySpec spec;
  std::string error;

  ASSERT_TRUE(PropertySpec::parseFromString("assertions\n", spec, error))
      << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 1u);
  EXPECT_EQ(spec.rules()[0].kind, PropertyKind::Assertions);
  EXPECT_EQ(spec.rules()[0].type, PropertyType::CHECK);
}

TEST(PropertySpecTest, ParseMultipleRules) {
  PropertySpec spec;
  std::string error;
  
  std::string content = 
    "CHECK( init(main()), LTL(G valid-free) )\n"
    "CHECK( init(main()), LTL(G valid-deref) )\n"
    "CHECK( init(main()), LTL(G valid-memtrack) )\n";
  
  ASSERT_TRUE(PropertySpec::parseFromString(content, spec, error)) << error;
  ASSERT_FALSE(spec.empty());
  ASSERT_EQ(spec.rules().size(), 3u);
  for (const auto &rule : spec.rules()) {
    EXPECT_EQ(rule.kind, PropertyKind::MemSafety);
  }
}

TEST(PropertySpecTest, RejectMalformedStructuredProperty) {
  PropertySpec spec;
  std::string error;

  std::string content = "COVER( init(main()), FQL(COVER EDGES(@CALL()) )\n";
  ASSERT_FALSE(PropertySpec::parseFromString(content, spec, error));
  EXPECT_FALSE(error.empty());
}

TEST(PropertySpecTest, ParseEmptyFile) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "# Comment only\n\n";
  ASSERT_FALSE(PropertySpec::parseFromString(content, spec, error));
  EXPECT_TRUE(spec.empty());
}

TEST(PropertySpecTest, ParseInvalidSyntax) {
  PropertySpec spec;
  std::string error;
  
  std::string content = "INVALID SYNTAX\n";
  ASSERT_FALSE(PropertySpec::parseFromString(content, spec, error));
  EXPECT_FALSE(error.empty());
}

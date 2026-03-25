#include "Verification/Backend/Backend.h"

#include <algorithm>

#include <gtest/gtest.h>

using namespace lotus::verification::backend;

TEST(BackendRegistryTest, AvailableBackends) {
  BackendRegistry &reg = BackendRegistry::instance();
  auto backends = reg.availableBackends();
  
  EXPECT_GT(backends.size(), 0u);
  EXPECT_NE(std::find(backends.begin(), backends.end(), "seahorn"), backends.end());
  EXPECT_NE(std::find(backends.begin(), backends.end(), "clam"), backends.end());
}

TEST(BackendRegistryTest, CreateBackend) {
  BackendRegistry &reg = BackendRegistry::instance();
  
  auto seahorn = reg.create("seahorn");
  ASSERT_NE(seahorn, nullptr);
  EXPECT_STREQ(seahorn->name(), "seahorn");
  
  auto clam = reg.create("clam");
  ASSERT_NE(clam, nullptr);
  EXPECT_STREQ(clam->name(), "clam");
  
  auto invalid = reg.create("nonexistent");
  EXPECT_EQ(invalid, nullptr);
}

TEST(BackendRegistryTest, RecommendBackends) {
  BackendRegistry &reg = BackendRegistry::instance();
  
  auto memSafetyBackends = reg.recommend(PropertyClass::MemSafety);
  EXPECT_GT(memSafetyBackends.size(), 0u);
  
  auto reachabilityBackends = reg.recommend(PropertyClass::Reachability);
  EXPECT_GT(reachabilityBackends.size(), 0u);
}

TEST(BackendTest, SeahornSupportsAllProperties) {
  BackendRegistry &reg = BackendRegistry::instance();
  auto backend = reg.create("seahorn");
  ASSERT_NE(backend, nullptr);
  
  EXPECT_TRUE(backend->supports(PropertyClass::Reachability));
  EXPECT_TRUE(backend->supports(PropertyClass::MemSafety));
  EXPECT_TRUE(backend->supports(PropertyClass::Overflow));
  EXPECT_TRUE(backend->supports(PropertyClass::Termination));
}

TEST(BackendTest, ParseSeahornResult) {
  BackendRegistry &reg = BackendRegistry::instance();
  auto backend = reg.create("seahorn");
  ASSERT_NE(backend, nullptr);
  
  // Test safe result
  VerificationResultInfo safe = backend->parseResult("unsat\n", 0);
  EXPECT_EQ(safe.result, VerificationResult::True);
  EXPECT_TRUE(safe.isSafe());
  
  // Test unsafe result
  VerificationResultInfo unsafe = backend->parseResult("sat\nError found\n", 0);
  EXPECT_EQ(unsafe.result, VerificationResult::False);
  EXPECT_TRUE(unsafe.hasError());
  
  // Test timeout
  VerificationResultInfo timeout = backend->parseResult("timeout\n", 124);
  EXPECT_EQ(timeout.result, VerificationResult::Timeout);
}

TEST(BackendTest, ParseClamResult) {
  BackendRegistry &reg = BackendRegistry::instance();
  auto backend = reg.create("clam");
  ASSERT_NE(backend, nullptr);
  
  VerificationResultInfo safe = backend->parseResult("safe\n", 0);
  EXPECT_EQ(safe.result, VerificationResult::True);
  
  VerificationResultInfo unsafe = backend->parseResult("unsafe\n", 0);
  EXPECT_EQ(unsafe.result, VerificationResult::False);
}

TEST(PropertyClassTest, ParsePropertyClass) {
  EXPECT_EQ(parsePropertyClass("unreach-call"), PropertyClass::Reachability);
  EXPECT_EQ(parsePropertyClass("memsafety"), PropertyClass::MemSafety);
  EXPECT_EQ(parsePropertyClass("overflow"), PropertyClass::Overflow);
  EXPECT_EQ(parsePropertyClass("termination"), PropertyClass::Termination);
  EXPECT_EQ(parsePropertyClass("unknown"), PropertyClass::Unknown);
}

TEST(PropertyClassTest, ToStringPropertyClass) {
  EXPECT_EQ(toString(PropertyClass::Reachability), "reachability");
  EXPECT_EQ(toString(PropertyClass::MemSafety), "memsafety");
  EXPECT_EQ(toString(PropertyClass::Overflow), "overflow");
}

TEST(VerificationResultTest, ParseResultFromString) {
  EXPECT_EQ(parseResultFromString("true"), VerificationResult::True);
  EXPECT_EQ(parseResultFromString("safe"), VerificationResult::True);
  EXPECT_EQ(parseResultFromString("false"), VerificationResult::False);
  EXPECT_EQ(parseResultFromString("unsafe"), VerificationResult::False);
  EXPECT_EQ(parseResultFromString("timeout"), VerificationResult::Timeout);
  EXPECT_EQ(parseResultFromString("error"), VerificationResult::Error);
}

TEST(VerificationResultTest, ToStringResult) {
  EXPECT_EQ(toString(VerificationResult::True), "true");
  EXPECT_EQ(toString(VerificationResult::False), "false");
  EXPECT_EQ(toString(VerificationResult::Unknown), "unknown");
  EXPECT_EQ(toString(VerificationResult::Timeout), "timeout");
  EXPECT_EQ(toString(VerificationResult::Error), "error");
}

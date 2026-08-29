#include "Dataflow/Mono/Domains/AvailableExpressionsDomain.h"
#include "Dataflow/Mono/Domains/ConstantPropagationDomain.h"
#include "Dataflow/Mono/Domains/FullConstantPropagationDomain.h"
#include "Dataflow/Mono/Domains/LiveVariablesDomain.h"
#include "Dataflow/Mono/Domains/ReachabilityDomain.h"
#include "Dataflow/Mono/Domains/ReachingDefinitionsDomain.h"
#include "Dataflow/Mono/Domains/TaintDomain.h"
#include "Dataflow/Mono/Domains/UninitializedVariablesDomain.h"

#include <cstdint>

#include <gtest/gtest.h>
#include <llvm/IR/Instruction.h>

namespace {

template <typename DomainT>
void expectJoinSemilatticeLaws(const DomainT &Domain,
                               const typename DomainT::value_type &X,
                               const typename DomainT::value_type &Y,
                               const typename DomainT::value_type &Z) {
  const auto Bottom = Domain.bottom();
  EXPECT_TRUE(Domain.equal(Domain.join(Bottom, X), X));
  EXPECT_TRUE(Domain.equal(Domain.join(X, Bottom), X));
  EXPECT_TRUE(Domain.equal(Domain.join(X, X), X));
  EXPECT_TRUE(Domain.equal(Domain.join(X, Y), Domain.join(Y, X)));
  EXPECT_TRUE(Domain.equal(Domain.join(Domain.join(X, Y), Z),
                           Domain.join(X, Domain.join(Y, Z))));
}

TEST(MonoDomain, UnionSetSatisfiesJoinSemilatticeLaws) {
  mono::LiveVariablesDomain Domain;
  mono::LiveVariablesDomain::value_type X;
  mono::LiveVariablesDomain::value_type Y;
  mono::LiveVariablesDomain::value_type Z;
  auto *A = reinterpret_cast<llvm::Value *>(std::uintptr_t{1});
  auto *B = reinterpret_cast<llvm::Value *>(std::uintptr_t{2});
  X.insert(A);
  Y.insert(B);
  Z.insert(A);
  Z.insert(B);
  expectJoinSemilatticeLaws(Domain, X, Y, Z);
}

TEST(MonoDomain, MustSetUsesUniverseAsBottomAndIntersectionAsJoin) {
  llvm::ArrayRef<llvm::Value *> NoOperands;
  mono::AvailableExpression A(llvm::Instruction::Add, NoOperands);
  mono::AvailableExpression B(llvm::Instruction::Mul, NoOperands);
  mono::AvailableExpressionsDomain::value_type Universe{A, B};
  mono::AvailableExpressionsDomain Domain(Universe);
  mono::AvailableExpressionsDomain::value_type OnlyA{A};
  mono::AvailableExpressionsDomain::value_type OnlyB{B};

  EXPECT_TRUE(Domain.equal(Domain.bottom(), Universe));
  EXPECT_TRUE(Domain.equal(Domain.join(Universe, OnlyA), OnlyA));
  EXPECT_TRUE(Domain.join(OnlyA, OnlyB).empty());
  expectJoinSemilatticeLaws(Domain, Universe, OnlyA, OnlyB);
}

TEST(MonoDomain, ConstantPropagationHasDistinctUnreachableBottom) {
  mono::ConstantPropagationDomain Domain;
  auto *Key = reinterpret_cast<const llvm::Value *>(std::uintptr_t{1});
  mono::ConstantPropagationMap One{
      {Key, {mono::ConstantPropagationTag::Const, 1}}};
  mono::ConstantPropagationMap Two{
      {Key, {mono::ConstantPropagationTag::Const, 2}}};
  mono::ConstantPropagationMap Unknown;

  EXPECT_FALSE(Domain.equal(Domain.bottom(), Unknown));
  EXPECT_TRUE(Domain.equal(Domain.join(Domain.bottom(), One), One));
  EXPECT_TRUE(Domain.equal(Domain.join(One, Two), Unknown));
  expectJoinSemilatticeLaws(Domain, One, Two, Unknown);
}

TEST(MonoDomain, FullConstantPropagationUsesUnreachableAsBottom) {
  mono::FullConstantPropagationDomain Domain;
  auto *Key = reinterpret_cast<const llvm::Value *>(std::uintptr_t{1});
  mono::FullConstantPropagationState One;
  One.Unreachable = false;
  One.Values[Key] = mono::FullConstantValue::constant(1);
  mono::FullConstantPropagationState Two;
  Two.Unreachable = false;
  Two.Values[Key] = mono::FullConstantValue::constant(2);
  auto Joined = Domain.join(One, Two);

  EXPECT_TRUE(Domain.bottom().Unreachable);
  EXPECT_TRUE(Domain.equal(Domain.join(Domain.bottom(), One), One));
  ASSERT_EQ(Joined.Values.count(Key), 1u);
  EXPECT_EQ(Joined.Values.at(Key).Tag, mono::FullConstantTag::Top);
  expectJoinSemilatticeLaws(Domain, One, Two, Joined);
}

static_assert(mono::IsMonoAbstractDomain<mono::LiveVariablesDomain>::value,
              "live variables must satisfy the Mono domain contract");
static_assert(
    mono::IsMonoAbstractDomain<mono::AvailableExpressionsDomain>::value,
    "available expressions must satisfy the Mono domain contract");
static_assert(
    mono::IsMonoAbstractDomain<mono::ConstantPropagationDomain>::value,
    "constant propagation must satisfy the Mono domain contract");
static_assert(
    mono::IsMonoAbstractDomain<mono::FullConstantPropagationDomain>::value,
    "full constant propagation must satisfy the Mono domain contract");
static_assert(mono::IsMonoAbstractDomain<mono::ReachabilityDomain>::value,
              "reachability must satisfy the Mono domain contract");
static_assert(
    mono::IsMonoAbstractDomain<mono::ReachingDefinitionsDomain>::value,
    "reaching definitions must satisfy the Mono domain contract");
static_assert(mono::IsMonoAbstractDomain<mono::TaintDomain>::value,
              "taint must satisfy the Mono domain contract");
static_assert(
    mono::IsMonoAbstractDomain<mono::UninitializedVariablesDomain>::value,
    "uninitialized variables must satisfy the Mono domain contract");

} // namespace

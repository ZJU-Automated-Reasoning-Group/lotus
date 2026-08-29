#include "Dataflow/APA/Core/AbstractDomain.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"
#include "Dataflow/APA/Domains/AvailableExpressionsDomain.h"
#include "Dataflow/APA/Domains/ConstantPropagationDomain.h"
#include "Dataflow/APA/Domains/LiveVariablesDomain.h"
#include "Dataflow/APA/Domains/LocksetDomain.h"
#include "Dataflow/APA/Domains/NonNullDomain.h"
#include "Dataflow/APA/Domains/ReachabilityDomain.h"
#include "Dataflow/APA/Domains/ReachingDefinitionsDomain.h"
#include "Dataflow/APA/Domains/SignDomain.h"
#include "Dataflow/APA/Domains/UninitializedVariablesDomain.h"
#include "Dataflow/APA/Domains/VeryBusyExpressionsDomain.h"

#include <cstdint>

#include <gtest/gtest.h>

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

TEST(APADomain, ReachabilitySatisfiesJoinSemilatticeLaws) {
  elimination::ReachabilityDomain Domain;
  expectJoinSemilatticeLaws(Domain, false, true, false);
}

TEST(APADomain, UnionSetSatisfiesJoinSemilatticeLaws) {
  elimination::LiveVariablesDomain Domain;
  elimination::LiveVariablesFact X;
  elimination::LiveVariablesFact Y;
  elimination::LiveVariablesFact Z;
  auto *A = reinterpret_cast<const llvm::Value *>(std::uintptr_t{1});
  auto *B = reinterpret_cast<const llvm::Value *>(std::uintptr_t{2});
  X.insert(A);
  Y.insert(B);
  Z.insert(A);
  Z.insert(B);
  expectJoinSemilatticeLaws(Domain, X, Y, Z);
}

TEST(APADomain, MustSetUsesUniverseAsBottomAndIntersectionAsJoin) {
  elimination::ExpressionKey A;
  A.Opcode = 1;
  elimination::ExpressionKey B;
  B.Opcode = 2;
  elimination::AvailableExpressionsFact Universe{A, B};
  elimination::AvailableExpressionsDomain Domain(Universe);
  elimination::AvailableExpressionsFact OnlyA{A};
  elimination::AvailableExpressionsFact OnlyB{B};

  EXPECT_TRUE(Domain.equal(Domain.bottom(), Universe));
  EXPECT_TRUE(Domain.equal(Domain.join(Universe, OnlyA), OnlyA));
  EXPECT_TRUE(Domain.join(OnlyA, OnlyB).empty());
  expectJoinSemilatticeLaws(Domain, Universe, OnlyA, OnlyB);
}

TEST(APADomain, ConstantPropagationTreatsMissingUnknownAsBottom) {
  elimination::ConstantPropagationDomain Domain;
  auto *Key = reinterpret_cast<const llvm::Value *>(std::uintptr_t{1});
  elimination::ConstantPropagationMap Unknown;
  elimination::ConstantPropagationMap Overdefined{
      {Key, llvm::ValueLatticeElement::getOverdefined()}};

  EXPECT_TRUE(
      Domain.equal(Domain.join(Domain.bottom(), Overdefined), Overdefined));
  expectJoinSemilatticeLaws(Domain, Unknown, Overdefined, Unknown);
}

TEST(APADomain, SignDomainSatisfiesJoinSemilatticeLaws) {
  elimination::SignDomain Domain;
  auto *Key = reinterpret_cast<const llvm::Value *>(std::uintptr_t{1});
  elimination::SignMap Negative{{Key, elimination::SignValue::negative()}};
  elimination::SignMap Positive{{Key, elimination::SignValue::positive()}};
  elimination::SignMap Either{{Key, elimination::SignValue::nonZero()}};
  expectJoinSemilatticeLaws(Domain, Negative, Positive, Either);
}

TEST(APADomain, UninitializedVariablesIsAMayUnionDomain) {
  elimination::UninitializedVariablesDomain Domain;
  elimination::UninitVariablesFact X;
  elimination::UninitVariablesFact Y;
  auto *A = reinterpret_cast<llvm::Value *>(std::uintptr_t{1});
  auto *B = reinterpret_cast<llvm::Value *>(std::uintptr_t{2});
  X.insert(A);
  Y.insert(B);
  expectJoinSemilatticeLaws(Domain, X, Y, Domain.join(X, Y));
}

static_assert(
    elimination::IsAPAAbstractDomain<elimination::ReachabilityDomain>::value,
    "reachability must satisfy the APA domain contract");
static_assert(elimination::IsAPAAbstractDomain<
                  elimination::AvailableExpressionsDomain>::value,
              "available expressions must satisfy the APA domain contract");
static_assert(elimination::IsAPAAbstractDomain<
                  elimination::ConstantPropagationDomain>::value,
              "constant propagation must satisfy the APA domain contract");
static_assert(
    elimination::IsAPAAbstractDomain<elimination::AffineRelationDomain>::value,
    "affine relations must satisfy the APA domain contract");
static_assert(
    elimination::IsAPAAbstractDomain<elimination::LiveVariablesDomain>::value,
    "live variables must satisfy the APA domain contract");
static_assert(
    elimination::IsAPAAbstractDomain<elimination::LocksetDomain>::value,
    "lockset must satisfy the APA domain contract");
static_assert(
    elimination::IsAPAAbstractDomain<elimination::NonNullDomain>::value,
    "nonnull must satisfy the APA domain contract");
static_assert(elimination::IsAPAAbstractDomain<
                  elimination::ReachingDefinitionsDomain>::value,
              "reaching definitions must satisfy the APA domain contract");
static_assert(elimination::IsAPAAbstractDomain<elimination::SignDomain>::value,
              "sign must satisfy the APA domain contract");
static_assert(elimination::IsAPAAbstractDomain<
                  elimination::UninitializedVariablesDomain>::value,
              "uninitialized variables must satisfy the APA domain contract");
static_assert(elimination::IsAPAAbstractDomain<
                  elimination::VeryBusyExpressionsDomain>::value,
              "very busy expressions must satisfy the APA domain contract");

} // namespace

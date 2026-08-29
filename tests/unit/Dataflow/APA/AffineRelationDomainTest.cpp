#include "Dataflow/APA/Domains/AffineRelationDomain.h"

#include "Dataflow/APA/Analyses/Inter/AffineEqualities.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::parseModule;

elimination::AffineRelationVocabulary buildVocabulary(const llvm::Module &M) {
  elimination::AffineRelationVocabulary vocab;
  std::unordered_set<const llvm::Value *> seen;
  auto record = [&](const llvm::Value *value) {
    if (seen.insert(value).second)
      vocab.values.push_back(value);
  };
  for (const auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (const auto &Arg : F.args()) {
      if (Arg.getType()->isIntegerTy() &&
          Arg.getType()->getIntegerBitWidth() <= 64)
        record(&Arg);
    }
    for (const auto &BB : F) {
      for (const auto &I : BB) {
        if (I.getType()->isIntegerTy() &&
            I.getType()->getIntegerBitWidth() <= 64)
          record(&I);
      }
    }
  }
  for (unsigned i = 0; i < vocab.values.size(); ++i) {
    vocab.indices[vocab.values[i]] = i;
    vocab.actualBitWidths[vocab.values[i]] =
        vocab.values[i]->getType()->getIntegerBitWidth();
  }
  return vocab;
}

using Matrix = std::vector<std::vector<llvm::APInt>>;

Matrix makeMatrix(unsigned bitWidth,
                  std::initializer_list<std::initializer_list<uint64_t>> rows) {
  Matrix out;
  for (auto row : rows) {
    out.emplace_back();
    for (uint64_t value : row)
      out.back().emplace_back(bitWidth, value);
  }
  return out;
}

bool hasMatrix(const std::vector<Matrix> &matrices, const Matrix &expected) {
  return std::find(matrices.begin(), matrices.end(), expected) !=
         matrices.end();
}

llvm::APInt dot(const std::vector<llvm::APInt> &lhs,
                const std::vector<llvm::APInt> &rhs) {
  llvm::APInt out(lhs.front().getBitWidth(), 0);
  for (size_t i = 0; i < lhs.size(); ++i)
    out += lhs[i] * rhs[i];
  return out;
}

bool rowsAreOrthogonal(const Matrix &lhs, const Matrix &rhs) {
  for (const auto &lhsRow : lhs) {
    for (const auto &rhsRow : rhs) {
      if (!dot(lhsRow, rhsRow).isZero())
        return false;
    }
  }
  return true;
}

Matrix multiply(const Matrix &lhs, const Matrix &rhs, unsigned bitWidth) {
  if (lhs.empty() || rhs.empty())
    return {};
  Matrix out(lhs.size(), std::vector<llvm::APInt>(rhs.front().size(),
                                                  llvm::APInt(bitWidth, 0)));
  for (size_t r = 0; r < lhs.size(); ++r) {
    for (size_t k = 0; k < rhs.size(); ++k) {
      for (size_t c = 0; c < rhs.front().size(); ++c)
        out[r][c] += lhs[r][k] * rhs[k][c];
    }
  }
  return out;
}

} // namespace

TEST(AffineRelationDomain, IdentityEqualsItself) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto id1 = elimination::AffineRelationDomain::identity();
  auto id2 = elimination::AffineRelationDomain::identity();
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(id1, id2));
}

TEST(AffineRelationDomain, ComposeWithIdentityPreservesAssignment) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = elimination::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto composed = elimination::AffineRelationDomain::extend(
      assign, elimination::AffineRelationDomain::identity());
  auto state = elimination::materializeAffineExpressions(composed);

  auto It = state.values.find(Y);
  ASSERT_NE(It, state.values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 4);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(X), 1);
}

TEST(AffineRelationDomain, GuardMaterializesConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %tag) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *Tag = &*module->getFunction("f")->arg_begin();
  auto guarded = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), Tag, 7);
  auto state = elimination::materializeAffineExpressions(guarded);

  auto It = state.values.find(Tag);
  ASSERT_NE(It, state.values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 7);
  EXPECT_TRUE(It->second.terms.empty());
}

TEST(AffineRelationDomain, BottomIsDistinctFromIdentity) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto bottom = elimination::AffineRelationDomain::zero();
  auto id = elimination::AffineRelationDomain::identity();
  EXPECT_FALSE(elimination::AffineRelationDomain::equal(bottom, id));
  EXPECT_TRUE(elimination::AffineRelationDomain::isBottom(bottom));
  EXPECT_FALSE(elimination::AffineRelationDomain::isBottom(id));
}

TEST(AffineRelationDomain, TopContainsIdentityAndBottom) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto top = elimination::AffineRelationDomain::top();
  auto id = elimination::AffineRelationDomain::identity();
  auto bottom = elimination::AffineRelationDomain::zero();

  EXPECT_TRUE(elimination::AffineRelationDomain::contains(top, id));
  EXPECT_TRUE(elimination::AffineRelationDomain::contains(top, bottom));
  EXPECT_TRUE(elimination::AffineRelationDomain::contains(id, bottom));
  EXPECT_FALSE(elimination::AffineRelationDomain::contains(id, top));
}

TEST(AffineRelationDomain, AffineGeneratorRoundTripPreservesKSRelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto relation =
      elimination::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto ag = elimination::AffineRelationDomain::toAffineGenerator(relation);
  auto roundTrip = elimination::AffineRelationDomain::fromAffineGenerator(ag);

  EXPECT_TRUE(ag.exact);
  ASSERT_FALSE(ag.generators.empty());
  EXPECT_FALSE(ag.generators.begin()->second.empty());
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(relation, roundTrip));
}

TEST(AffineRelationDomain, AffineGeneratorJoinUsesKSJoinSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = elimination::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto id = elimination::AffineRelationDomain::identity();
  auto joined = elimination::AffineRelationDomain::joinAffineGenerators(
      elimination::AffineRelationDomain::toAffineGenerator(assign),
      elimination::AffineRelationDomain::toAffineGenerator(id));

  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::fromAffineGenerator(joined),
      elimination::AffineRelationDomain::combine(assign, id)));
}

TEST(AffineRelationDomain, PublicDualizePerpComputesOrthogonalComplement) {
  Matrix constraints = makeMatrix(4, {
                                         {1, 15, 0},
                                     });

  auto decomposition =
      elimination::AffineRelationDomain::diagonalDecompose(constraints, 4);
  auto dual = elimination::AffineRelationDomain::dualizePerp(constraints, 4, 3);
  auto roundTrip = elimination::AffineRelationDomain::dualizePerp(dual, 4, 3);

  EXPECT_EQ(decomposition.bitWidth, 4u);
  EXPECT_EQ(decomposition.dual, dual);
  EXPECT_TRUE(rowsAreOrthogonal(constraints, dual));
  EXPECT_TRUE(rowsAreOrthogonal(dual, roundTrip));
  EXPECT_EQ(roundTrip, constraints);
  EXPECT_EQ(multiply(multiply(decomposition.left, decomposition.diagonal, 4),
                     decomposition.right, 4),
            constraints);
  EXPECT_EQ(multiply(decomposition.left, decomposition.leftInverse, 4),
            makeMatrix(4, {{1}}));
  EXPECT_EQ(multiply(decomposition.right, decomposition.rightInverse, 4),
            makeMatrix(4, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}));
}

TEST(AffineRelationDomain, DiagonalDecomposeTracksRectangularFactors) {
  Matrix constraints = makeMatrix(4, {
                                         {2, 6, 4},
                                         {0, 4, 8},
                                     });

  auto decomposition =
      elimination::AffineRelationDomain::diagonalDecompose(constraints, 4);
  auto dual = elimination::AffineRelationDomain::dualizePerp(constraints, 4, 3);

  EXPECT_EQ(multiply(multiply(decomposition.left, decomposition.diagonal, 4),
                     decomposition.right, 4),
            constraints);
  EXPECT_EQ(multiply(decomposition.left, decomposition.leftInverse, 4),
            makeMatrix(4, {{1, 0}, {0, 1}}));
  EXPECT_EQ(multiply(decomposition.right, decomposition.rightInverse, 4),
            makeMatrix(4, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}));
  EXPECT_EQ(decomposition.dual, dual);
  EXPECT_TRUE(rowsAreOrthogonal(constraints, dual));
}

TEST(AffineRelationDomain, AffineGeneratorKSRoundTripPreservesGeneratorSpace) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  elimination::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0},
                                       {0, 1, 1},
                                   });

  auto relation = elimination::AffineRelationDomain::fromAffineGenerator(ag);
  auto roundTripAG = elimination::AffineRelationDomain::toAffineGenerator(relation);
  auto roundTripKS =
      elimination::AffineRelationDomain::fromAffineGenerator(roundTripAG);

  ASSERT_FALSE(roundTripAG.generators.empty());
  EXPECT_FALSE(roundTripAG.generators.begin()->second.empty());
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(relation, roundTripKS));
}

TEST(AffineRelationDomain, MOSRoundTripPreservesKSRelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto relation =
      elimination::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto mos = elimination::AffineRelationDomain::toMOSWithMakeExplicit(relation);
  auto roundTrip = elimination::AffineRelationDomain::fromMOS(mos);

  EXPECT_TRUE(mos.exact);
  EXPECT_EQ(mos.kind, elimination::MOSRelation::ConversionKind::MakeExplicit);
  ASSERT_FALSE(mos.transformers.empty());
  EXPECT_FALSE(mos.transformers.begin()->second.empty());
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(relation, roundTrip));
}

TEST(AffineRelationDomain, MOSToKSConvertsExplicitAffineTransformer) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *X = &*module->getFunction("f")->arg_begin();
  elimination::MOSRelation mos;
  mos.transformers[4] = {
      makeMatrix(4, {{1, 3}, {0, 5}}),
  };

  auto relation = elimination::AffineRelationDomain::fromMOS(mos);
  auto state = elimination::materializeAffineExpressions(relation);

  auto It = state.values.find(X);
  ASSERT_NE(It, state.values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 3);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(X), 5);
}

TEST(AffineRelationDomain, GuardedKSToMOSVariantsProduceDistinctTransformers) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  elimination::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0},
                                       {0, 4, 12},
                                   });
  ag.relation = elimination::AffineRelationDomain::fromAffineGenerator(ag);

  auto havoc =
      elimination::AffineRelationDomain::toMOSWithHavocedPreStateGuards(ag.relation);
  auto explicitMOS =
      elimination::AffineRelationDomain::toMOSWithMakeExplicit(ag.relation);

  ASSERT_EQ(havoc.transformers.count(4), 1u);
  ASSERT_EQ(explicitMOS.transformers.count(4), 1u);

  EXPECT_TRUE(
      hasMatrix(havoc.transformers.at(4), makeMatrix(4, {{1, 0}, {0, 0}})));
  EXPECT_TRUE(
      hasMatrix(havoc.transformers.at(4), makeMatrix(4, {{0, 4}, {0, 0}})));
  EXPECT_TRUE(hasMatrix(explicitMOS.transformers.at(4),
                        makeMatrix(4, {{1, 0}, {0, 3}})));
  EXPECT_FALSE(
      hasMatrix(havoc.transformers.at(4), makeMatrix(4, {{1, 0}, {0, 3}})));
}

TEST(AffineRelationDomain, MakeExplicitHandlesSkippedPreStateColumns) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x, i4 %y) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  elimination::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0, 0, 0},
                                       {0, 0, 1, 0, 7},
                                   });
  ag.relation = elimination::AffineRelationDomain::fromAffineGenerator(ag);

  auto explicitMOS =
      elimination::AffineRelationDomain::toMOSWithMakeExplicit(ag.relation);

  ASSERT_EQ(explicitMOS.transformers.count(4), 1u);
  ASSERT_FALSE(explicitMOS.transformers.at(4).empty());
  EXPECT_FALSE(elimination::AffineRelationDomain::isBottom(
      elimination::AffineRelationDomain::fromMOS(explicitMOS)));
}

TEST(AffineRelationDomain, MakeExplicitSplitsEvenPreStateLeadingValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  elimination::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0},
                                       {0, 2, 6},
                                   });
  ag.relation = elimination::AffineRelationDomain::fromAffineGenerator(ag);

  auto explicitMOS =
      elimination::AffineRelationDomain::toMOSWithMakeExplicit(ag.relation);

  ASSERT_EQ(explicitMOS.transformers.count(4), 1u);
  EXPECT_TRUE(hasMatrix(explicitMOS.transformers.at(4),
                        makeMatrix(4, {{1, 0}, {0, 3}})));
}

TEST(AffineRelationDomain, MOSJoinUsesKSJoinSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = elimination::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto id = elimination::AffineRelationDomain::identity();
  auto joined = elimination::AffineRelationDomain::joinMOS(
      elimination::AffineRelationDomain::toMOS(assign),
      elimination::AffineRelationDomain::toMOS(id));

  EXPECT_EQ(joined.kind, elimination::MOSRelation::ConversionKind::Direct);
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::fromMOS(joined),
      elimination::AffineRelationDomain::combine(assign, id)));
}

TEST(AffineRelationDomain, SubtractUsesAffineGeneratorResidual) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = elimination::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto id = elimination::AffineRelationDomain::identity();
  auto residual = elimination::AffineRelationDomain::subtract(assign, id);

  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::combine(residual, id),
      elimination::AffineRelationDomain::combine(assign, id)));
  EXPECT_TRUE(elimination::AffineRelationDomain::contains(assign, residual));
  EXPECT_TRUE(elimination::AffineRelationDomain::isBottom(
      elimination::AffineRelationDomain::subtract(assign, assign)));
}

TEST(AffineRelationDomain, CondCombineRespectsBooleanGuard) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto id = elimination::AffineRelationDomain::identity();
  auto bottom = elimination::AffineRelationDomain::zero();

  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::condCombine(true, id, bottom), id));
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::condCombine(false, id, bottom), bottom));
}

TEST(AffineRelationDomain, MeetAddsConstraintsExactly) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x, i32 %y) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *X = &*ArgIt;
  ++ArgIt;
  auto *Y = &*ArgIt;

  auto xIsThree = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), X, 3);
  auto yIsFive = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), Y, 5);
  auto both = elimination::AffineRelationDomain::meet(xIsThree, yIsFive);
  auto state = elimination::materializeAffineExpressions(both);

  ASSERT_NE(state.values.find(X), state.values.end());
  ASSERT_NE(state.values.find(Y), state.values.end());
  EXPECT_EQ(state.values.at(X).constant, 3);
  EXPECT_EQ(state.values.at(Y).constant, 5);
}

TEST(AffineRelationDomain, ProjectAndHavocRemoveSelectedVocabulary) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x, i32 %y) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *X = &*ArgIt;
  ++ArgIt;
  auto *Y = &*ArgIt;

  auto relation = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), X, 7);
  relation = elimination::AffineRelationDomain::addPrecondition(relation, Y, 11);

  auto onlyX = elimination::AffineRelationDomain::projectOnto(relation, {X});
  auto onlyXState = elimination::materializeAffineExpressions(onlyX);
  ASSERT_NE(onlyXState.values.find(X), onlyXState.values.end());
  EXPECT_EQ(onlyXState.values.at(X).constant, 7);
  EXPECT_EQ(onlyXState.values.find(Y), onlyXState.values.end());

  auto withoutY = elimination::AffineRelationDomain::havoc(relation, Y);
  auto withoutYState = elimination::materializeAffineExpressions(withoutY);
  ASSERT_NE(withoutYState.values.find(X), withoutYState.values.end());
  EXPECT_EQ(withoutYState.values.at(X).constant, 7);
  EXPECT_EQ(withoutYState.values.find(Y), withoutYState.values.end());
}

TEST(AffineRelationDomain, SizeCountsSatisfyingTwoVocabularySolutions) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i8 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *X = &*module->getFunction("f")->arg_begin();
  auto idSize =
      elimination::AffineRelationDomain::size(elimination::AffineRelationDomain::identity());
  EXPECT_EQ(idSize.getZExtValue(), 256u);

  auto fixed = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), X, 42);
  auto fixedSize = elimination::AffineRelationDomain::size(fixed);
  EXPECT_EQ(fixedSize.getZExtValue(), 1u);

  auto bottomSize =
      elimination::AffineRelationDomain::size(elimination::AffineRelationDomain::zero());
  EXPECT_EQ(bottomSize.getZExtValue(), 0u);
}

TEST(AffineRelationDomain, MergePreservingLocalsKeepsCallerLocalValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %g, i32 %l) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  elimination::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *G = &*ArgIt;
  ++ArgIt;
  auto *L = &*ArgIt;

  auto callSite = elimination::AffineRelationDomain::extend(
      elimination::AffineRelationDomain::makeAffineAssignment(L, 2, {{L, 1}}),
      elimination::AffineRelationDomain::makeAffineAssignment(G, 1, {{G, 1}}));
  auto calleeExit = elimination::AffineRelationDomain::extend(
      elimination::AffineRelationDomain::makeAffineAssignment(L, 0, {{G, 1}}),
      elimination::AffineRelationDomain::makeAffineAssignment(G, 3, {{G, 1}}));

  auto merged = elimination::AffineRelationDomain::mergePreservingLocals(
      callSite, calleeExit, {L});
  auto state = elimination::materializeAffineExpressions(merged);

  ASSERT_NE(state.values.find(G), state.values.end());
  EXPECT_EQ(state.values.at(G).constant, 4);
  ASSERT_EQ(state.values.at(G).terms.size(), 1u);
  EXPECT_EQ(state.values.at(G).terms.at(G), 1);

  ASSERT_NE(state.values.find(L), state.values.end());
  EXPECT_EQ(state.values.at(L).constant, 2);
  ASSERT_EQ(state.values.at(L).terms.size(), 1u);
  EXPECT_EQ(state.values.at(L).terms.at(L), 1);
}

TEST(AffineRelationDomain, GenericProjectDropsConfiguredLocals) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %g, i32 %l) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *G = &*ArgIt;
  ++ArgIt;
  auto *L = &*ArgIt;
  vocab.localValues = {L};
  elimination::AffineRelationDomain::configure(&vocab);

  auto relation = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), G, 7);
  relation = elimination::AffineRelationDomain::addPrecondition(relation, L, 11);

  auto projected = elimination::AffineRelationDomain::project(relation);
  auto state = elimination::materializeAffineExpressions(projected);

  ASSERT_NE(state.values.find(G), state.values.end());
  EXPECT_EQ(state.values.at(G).constant, 7);
  EXPECT_EQ(state.values.find(L), state.values.end());
}

TEST(AffineRelationDomain, GenericProjectIsIdempotentAndOptional) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %g, i32 %l) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *G = &*ArgIt;
  ++ArgIt;
  auto *L = &*ArgIt;
  vocab.localValues = {L};
  elimination::AffineRelationDomain::configure(&vocab);

  auto first = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), G, 3);
  first = elimination::AffineRelationDomain::addPrecondition(first, L, 5);

  auto second = elimination::AffineRelationDomain::addPrecondition(
      elimination::AffineRelationDomain::identity(), G, 9);
  second = elimination::AffineRelationDomain::addPrecondition(second, L, 13);

  auto projectedFirst = elimination::AffineRelationDomain::project(first);
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::project(projectedFirst), projectedFirst));

  vocab.localValues.clear();
  elimination::AffineRelationDomain::configure(&vocab);
  EXPECT_TRUE(elimination::AffineRelationDomain::equal(
      elimination::AffineRelationDomain::project(second), second));
}

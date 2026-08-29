#include "Dataflow/NPA/LLVM/BackwardInterEngine.h"
#include "Dataflow/NPA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Inter/Interval.h"
#include "Dataflow/NPA/Analyses/Inter/LiveVariables.h"
#include "Dataflow/NPA/Analyses/Inter/MaybeUninitialized.h"
#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/Domains/PathTransferSummary.h"
#include "TestUtils/LLVMHelpers.h"

#include <cctype>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

template <typename T>
std::vector<const T *> statesForBlock(const std::map<npa::BlockKey, T> &facts,
                                      const llvm::BasicBlock *block) {
  std::vector<const T *> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(&entry.second);
  }
  return out;
}

llvm::APInt unionFactForBlock(const std::map<npa::BlockKey, llvm::APInt> &facts,
                              const llvm::BasicBlock *block) {
  bool found = false;
  llvm::APInt fact(1, 0);
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    if (!found) {
      fact = entry.second;
      found = true;
    } else {
      fact |= entry.second;
    }
  }
  EXPECT_TRUE(found);
  return fact;
}

template <typename Op, typename OpLess>
bool containsPath(const npa::PathTransferSummaryValue<Op, OpLess> &transfer,
                  std::initializer_list<Op> expected) {
  typename npa::PathTransferSummaryValue<Op, OpLess>::path_type path(expected);
  return transfer.paths.count(path) != 0;
}

llvm::APInt signedAPInt(unsigned bitWidth, int64_t value) {
  return llvm::APInt(bitWidth, static_cast<uint64_t>(value), true);
}

llvm::APInt unsignedAPInt(unsigned bitWidth, uint64_t value) {
  return llvm::APInt(bitWidth, value, false);
}

void expectConstValue(const npa::ConstantPropagationValue &value,
                      const llvm::APInt &expected) {
  EXPECT_EQ(value.tag, npa::ConstantPropagationTag::Const);
  EXPECT_EQ(value.constant.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.constant.eq(expected));
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> sortedPredicateTransitions(
    const npa::PredicateRelationDomain::value_type &relation) {
  auto transitions = npa::PredicateRelationDomain::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

using EntryHookDomain = npa::PathTransferSummary<char>;

struct ProjectedStringDomain {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }

  static bool equal(const value_type &lhs, const value_type &rhs) {
    return lhs == rhs;
  }

  static value_type combine(const value_type &lhs, const value_type &rhs) {
    value_type out = lhs;
    out.insert(rhs.begin(), rhs.end());
    return out;
  }

  static value_type ndetCombine(const value_type &lhs, const value_type &rhs) {
    return combine(lhs, rhs);
  }

  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }

  static value_type extend(const value_type &lhs, const value_type &rhs) {
    value_type out;
    for (const auto &x : lhs) {
      for (const auto &y : rhs)
        out.insert(x + y);
    }
    return out;
  }

  static value_type extend_lin(const value_type &lhs, const value_type &rhs) {
    return extend(lhs, rhs);
  }

  static value_type subtract(const value_type &lhs, const value_type &rhs) {
    value_type out;
    for (const auto &item : lhs) {
      if (!rhs.count(item))
        out.insert(item);
    }
    return out;
  }

  static value_type project(const value_type &value) {
    value_type out;
    for (const auto &path : value) {
      std::string projected;
      projected.reserve(path.size());
      for (char ch : path) {
        if (ch != 'l')
          projected.push_back(ch);
      }
      out.insert(std::move(projected));
    }
    return out;
  }
};

class EntryHookAnalysis {
public:
  using FactType = EntryHookDomain::value_type;
  using E = npa::E0<EntryHookDomain>;
  using Exp = npa::Exp0<EntryHookDomain>;

  FactType getEntryValue() const { return EntryHookDomain::one(); }

  E buildBlockEntryExpr(llvm::BasicBlock &BB, E inExpr) {
    char label = 'B';
    if (BB.hasName() && !BB.getName().empty())
      label = static_cast<char>(std::toupper(BB.getName().front()));
    return Exp::seq(EntryHookDomain::singleton(label), inExpr);
  }

  E getTransfer(llvm::Instruction &, E currentPath) { return currentPath; }

  FactType applySummary(const FactType &summary, const FactType &fact) {
    return EntryHookDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return EntryHookDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return EntryHookDomain::equal(lhs, rhs);
  }
};

struct LimitedBoolDomain {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr long max_linear_steps = 0;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type lhs, value_type rhs) { return lhs == rhs; }
  static value_type combine(value_type lhs, value_type rhs) {
    return lhs || rhs;
  }
  static value_type ndetCombine(value_type lhs, value_type rhs) {
    return combine(lhs, rhs);
  }
  static value_type condCombine(bool phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type extend(value_type lhs, value_type rhs) {
    return lhs && rhs;
  }
  static value_type extend_lin(value_type lhs, value_type rhs) {
    return extend(lhs, rhs);
  }
  static value_type subtract(value_type lhs, value_type rhs) {
    return lhs && !rhs;
  }
};

class LimitedBoolAnalysis {
public:
  using FactType = bool;
  using E = npa::E0<LimitedBoolDomain>;

  FactType getEntryValue() const { return true; }
  E getTransfer(llvm::Instruction &, E currentPath) const {
    return currentPath;
  }
  FactType applySummary(bool summary, bool fact) const {
    return summary && fact;
  }
  FactType joinFacts(bool lhs, bool rhs) const { return lhs || rhs; }
  bool factsEqual(bool lhs, bool rhs) const { return lhs == rhs; }
};

class ProjectedSummaryAnalysis {
public:
  using FactType = ProjectedStringDomain::value_type;
  using E = npa::E0<ProjectedStringDomain>;
  using Exp = npa::Exp0<ProjectedStringDomain>;

  FactType getEntryValue() const { return ProjectedStringDomain::one(); }

  E getTransfer(llvm::Instruction &inst, E currentPath) const {
    if (inst.getFunction() && inst.getFunction()->getName() == "callee" &&
        llvm::isa<llvm::ReturnInst>(&inst)) {
      return Exp::seq(ProjectedStringDomain::value_type{"l"}, currentPath);
    }
    return currentPath;
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return ProjectedStringDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return ProjectedStringDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return ProjectedStringDomain::equal(lhs, rhs);
  }
};

class PredicateProjectedLoopAnalysis {
public:
  using Domain = npa::PredicateRelationDomain;
  using FactType = Domain::value_type;
  using E = npa::E0<Domain>;
  using Exp = npa::Exp0<Domain>;

  FactType getEntryValue() const { return Domain::one(); }

  E getTransfer(llvm::Instruction &inst, E currentPath) const {
    if (inst.hasName() && inst.getName() == "set_local")
      return Exp::seq(Domain::assignConst(1, true), currentPath);
    return currentPath;
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return Domain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return Domain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return Domain::equal(lhs, rhs);
  }
};

using RecursiveSummaryDomain = npa::PathTransferSummary<char>;

class RecursivePropagationLimitedForwardAnalysis {
public:
  using FactType = RecursiveSummaryDomain::value_type;
  using E = npa::E0<RecursiveSummaryDomain>;

  FactType getEntryValue() const { return RecursiveSummaryDomain::one(); }

  E getTransfer(llvm::Instruction &, E currentPath) const {
    return currentPath;
  }

  FactType getCallEntryTransfer(const llvm::CallBase &,
                                const llvm::Function &) const {
    return RecursiveSummaryDomain::zero();
  }

  FactType getCallReturnTransfer(const llvm::CallBase &,
                                 const llvm::Function &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType getCallToReturnTransfer(const llvm::CallBase &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return RecursiveSummaryDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::equal(lhs, rhs);
  }

  long getMaxPropagationSteps() const { return 0; }
};

class RecursivePropagationLimitedBackwardAnalysis {
public:
  using FactType = RecursiveSummaryDomain::value_type;
  using E = npa::E0<RecursiveSummaryDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return RecursiveSummaryDomain::one();
  }

  E getTransfer(llvm::Instruction &, E currentPath) const {
    return currentPath;
  }

  FactType getCallReturnTransfer(const llvm::CallBase &,
                                 const llvm::Function &) const {
    return RecursiveSummaryDomain::zero();
  }

  FactType getCallEntryTransfer(const llvm::CallBase &,
                                const llvm::Function &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType getCallToReturnTransfer(const llvm::CallBase &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return RecursiveSummaryDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::equal(lhs, rhs);
  }

  long getMaxPropagationSteps() const { return 0; }
};

void expectIntervalPoint(const npa::Interval &value,
                         const llvm::APInt &expected,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  ASSERT_EQ(value.lower.getBitWidth(), expected.getBitWidth());
  ASSERT_EQ(value.upper.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.lower.eq(expected));
  EXPECT_TRUE(value.upper.eq(expected));
}

void expectIntervalRange(const npa::Interval &value, const llvm::APInt &lower,
                         const llvm::APInt &upper,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  ASSERT_EQ(value.lower.getBitWidth(), lower.getBitWidth());
  ASSERT_EQ(value.upper.getBitWidth(), upper.getBitWidth());
  EXPECT_TRUE(value.lower.eq(lower));
  EXPECT_TRUE(value.upper.eq(upper));
}

} // namespace

TEST(NPAInterproceduralClients, MaybeUninitializedFlowsThroughCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = alloca i32
      %r = call i32 @id(i32* %p)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  auto result = npa::InterMaybeUninitialized::run(*module);
  llvm::APInt entryFact =
      unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_GT(entryFact.countPopulation(), 0u);
}

TEST(NPAInterproceduralClients, MaybeUninitializedStoreClearsBeforeCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = alloca i32
      store i32 7, i32* %p
      %r = call i32 @id(i32* %p)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  auto result = npa::InterMaybeUninitialized::run(*module);
  llvm::APInt entryFact =
      unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_EQ(entryFact.countPopulation(), 0u);
}

TEST(NPAInterproceduralClients,
     GenericBlockEntryHookAppliesToEntryAndSuccessorBlocks) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      br label %mid

    mid:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  EntryHookAnalysis analysis;
  auto result =
      npa::InterEngine<EntryHookDomain, EntryHookAnalysis>::run(
          *module, analysis, false, npa::LinearStrategy::SCC);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto EntryIt = Main->begin();
  auto MidIt = std::next(Main->begin(), 1);
  auto ExitIt = std::next(Main->begin(), 2);
  ASSERT_NE(ExitIt, Main->end());

  auto entryStates = statesForBlock(result.blockEntryFacts, &*EntryIt);
  auto midStates = statesForBlock(result.blockEntryFacts, &*MidIt);
  auto exitStates = statesForBlock(result.blockEntryFacts, &*ExitIt);
  ASSERT_EQ(entryStates.size(), 1u);
  ASSERT_EQ(midStates.size(), 1u);
  ASSERT_EQ(exitStates.size(), 1u);

  EXPECT_TRUE(containsPath(*entryStates.front(), {'E'}));
  EXPECT_TRUE(containsPath(*midStates.front(), {'E', 'M'}));
  EXPECT_TRUE(containsPath(*exitStates.front(), {'E', 'M', 'E'}));
}

TEST(NPAInterproceduralClients,
     InterproceduralEngineReportsBoundedSolverResults) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  LimitedBoolAnalysis analysis;
  auto result =
      npa::InterEngine<LimitedBoolDomain, LimitedBoolAnalysis>::run(
          *module, analysis, false, npa::LinearStrategy::SCC);

  EXPECT_FALSE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.status.summary_solve.hit_limit);
  EXPECT_TRUE(result.status.summary_solve.hit_linear_limit);
  EXPECT_FALSE(result.status.summary_solve.hit_fixpoint_limit);
  EXPECT_TRUE(result.status.used_bounded_inner_solve);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.status.overall_hit_limit);
  EXPECT_TRUE(result.status.approximated);
}

TEST(NPAInterproceduralClients,
     ForwardEngineSeparatesExactSummarySolveFromPropagationLimitOnRecursion) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @recur() {
    entry:
      call void @recur()
      ret void
    }

    define void @main() {
    entry:
      call void @recur()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  RecursivePropagationLimitedForwardAnalysis analysis;
  auto result =
      npa::InterEngine<RecursiveSummaryDomain,
                                 RecursivePropagationLimitedForwardAnalysis>::
          run(*module, analysis, false, npa::LinearStrategy::SCC);

  auto *Recur = module->getFunction("recur");
  ASSERT_NE(Recur, nullptr);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.status.summary_solve.hit_limit);
  EXPECT_FALSE(result.status.used_bounded_inner_solve);
  EXPECT_FALSE(result.status.propagation_converged);
  EXPECT_TRUE(result.status.propagation_hit_limit);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.status.approximated);
  EXPECT_NE(result.summaries.find(npa::FunctionKey{Recur}),
            result.summaries.end());
}

TEST(NPAInterproceduralClients,
     ConstantPropagationTransfersArgumentsAcrossCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @add2(i32 %x) {
    entry:
      %y = add i32 %x, 2
      ret i32 %y
    }

    define i32 @main() {
    entry:
      %r = call i32 @add2(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Add2 = module->getFunction("add2");
  ASSERT_NE(Add2, nullptr);
  auto *Arg = &*Add2->arg_begin();

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, &Add2->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 5));
}

TEST(NPAInterproceduralClients, ConstantPropagationReportsSummaryOverflow) {
  llvm::LLVMContext ctx;
  std::ostringstream ir;
  ir << "define i32 @chain(i32 %x) {\n";
  ir << "entry:\n";
  ir << "  br label %b0\n";
  std::string current = "%x";
  for (unsigned i = 0; i < 321; ++i) {
    ir << "b" << i << ":\n";
    const std::string next = "%v" + std::to_string(i);
    ir << "  " << next << " = add i32 " << current << ", 1\n";
    ir << "  br label %" << (i == 320 ? "exit" : "b" + std::to_string(i + 1))
       << "\n";
    current = next;
  }
  ir << "exit:\n";
  ir << "  ret i32 " << current << "\n";
  ir << "}\n\n";
  ir << "define i32 @main() {\n";
  ir << "entry:\n";
  ir << "  %r = call i32 @chain(i32 0)\n";
  ir << "  ret i32 %r\n";
  ir << "}\n";

  const std::string moduleText = ir.str();
  auto module = parseModule(ctx, moduleText.c_str());
  ASSERT_NE(module, nullptr);

  auto result = npa::InterConstantPropagation::run(*module);

  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.status.used_summary_overflow);
  EXPECT_FALSE(result.status.used_fact_widening);
  EXPECT_TRUE(result.status.approximated);
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPAInterproceduralClients,
     InterproceduralClientsAcceptTensorStrategyOnDemand) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %r = call i32 @id(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterConstantPropagation::run(
      *module, false, npa::LinearStrategy::TensorProduct);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.status.summary_solve.hit_limit);
  EXPECT_FALSE(result.status.used_bounded_inner_solve);
  EXPECT_TRUE(result.status.overall_converged);
  EXPECT_FALSE(result.status.overall_hit_limit);
  EXPECT_FALSE(result.status.approximated);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 5));
}

TEST(NPAInterproceduralClients,
     PublicSummariesRemainUnprojectedWhenDomainProjects) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @callee()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  ProjectedSummaryAnalysis analysis;
  auto result = npa::InterEngine<
      ProjectedStringDomain,
      ProjectedSummaryAnalysis>::run(*module, analysis, false,
                                     npa::LinearStrategy::SCC);

  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Callee, nullptr);
  auto it = result.summaries.find(npa::FunctionKey{Callee});
  ASSERT_NE(it, result.summaries.end());
  EXPECT_EQ(it->second, (ProjectedStringDomain::value_type{"l"}));
}

TEST(NPAInterproceduralClients,
     DomainsWithProjectOnlyProjectCalleeSummariesAtCallSites) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @callee()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  ProjectedSummaryAnalysis analysis;
  auto result = npa::InterEngine<
      ProjectedStringDomain,
      ProjectedSummaryAnalysis>::run(*module, analysis, false,
                                     npa::LinearStrategy::SCC);

  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  auto main_it = result.summaries.find(npa::FunctionKey{Main});
  auto callee_it = result.summaries.find(npa::FunctionKey{Callee});
  ASSERT_NE(main_it, result.summaries.end());
  ASSERT_NE(callee_it, result.summaries.end());

  EXPECT_EQ(callee_it->second, (ProjectedStringDomain::value_type{"l"}));
  EXPECT_EQ(main_it->second, (ProjectedStringDomain::value_type{""}));
}

TEST(NPAInterproceduralClients,
     PredicateRelationTensorStrategyPreservesProjectedLoopSummarySemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @callee(i1 %cond) {
    entry:
      br label %loop

    loop:
      %set_local = xor i1 %cond, false
      br i1 %cond, label %loop, label %exit

    exit:
      ret void
    }

    define void @main(i1 %cond) {
    entry:
      call void @callee(i1 %cond)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  npa::PredicateRelationDomain::configure(2, 1);
  PredicateProjectedLoopAnalysis analysis;

  auto worklist = npa::InterEngine<
      npa::PredicateRelationDomain,
      PredicateProjectedLoopAnalysis>::run(*module, analysis, false,
                                           npa::LinearStrategy::SCC);

  auto tensor = npa::InterEngine<
      npa::PredicateRelationDomain,
      PredicateProjectedLoopAnalysis>::run(*module, analysis, false,
                                           npa::LinearStrategy::TensorProduct);

  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  auto worklist_main = worklist.summaries.find(npa::FunctionKey{Main});
  auto worklist_callee = worklist.summaries.find(npa::FunctionKey{Callee});
  auto tensor_main = tensor.summaries.find(npa::FunctionKey{Main});
  auto tensor_callee = tensor.summaries.find(npa::FunctionKey{Callee});
  ASSERT_NE(worklist_main, worklist.summaries.end());
  ASSERT_NE(worklist_callee, worklist.summaries.end());
  ASSERT_NE(tensor_main, tensor.summaries.end());
  ASSERT_NE(tensor_callee, tensor.summaries.end());

  EXPECT_TRUE(npa::PredicateRelationDomain::equal(worklist_main->second,
                                                  tensor_main->second));
  EXPECT_TRUE(npa::PredicateRelationDomain::equal(worklist_callee->second,
                                                  tensor_callee->second));
  EXPECT_EQ(sortedPredicateTransitions(tensor_main->second),
            sortedPredicateTransitions(npa::PredicateRelationDomain::one()));
  EXPECT_EQ(sortedPredicateTransitions(tensor_callee->second),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 2}, {1, 3}, {2, 2}, {3, 3}}));
}

TEST(NPAInterproceduralClients,
     TensorStrategyFallsBackForNonAdmissibleInterproceduralDomains) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %r = call i32 @id(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  testing::internal::CaptureStderr();
  auto result = npa::InterConstantPropagation::run(
      *module, true, npa::LinearStrategy::TensorProduct);
  std::string stderrOutput = testing::internal::GetCapturedStderr();

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto *Arg = &*Id->arg_begin();
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 5));
  EXPECT_NE(stderrOutput.find("falling back"), std::string::npos);
}

TEST(NPAInterproceduralClients, ConstantPropagationUsesLLVMIntegerSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %z = zext i1 true to i32
      %t = trunc i32 256 to i8
      %cmp = icmp ugt i32 -1, 1
      %q = udiv i32 -1, 2
      %s = lshr i32 -1, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Z = findInstructionByName(*Main, "z");
  auto *T = findInstructionByName(*Main, "t");
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Q = findInstructionByName(*Main, "q");
  auto *S = findInstructionByName(*Main, "s");
  ASSERT_NE(Z, nullptr);
  ASSERT_NE(T, nullptr);
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto ZIt = states.front()->values.find(Z);
  auto TIt = states.front()->values.find(T);
  auto CmpIt = states.front()->values.find(Cmp);
  auto QIt = states.front()->values.find(Q);
  auto SIt = states.front()->values.find(S);
  ASSERT_NE(ZIt, states.front()->values.end());
  ASSERT_NE(TIt, states.front()->values.end());
  ASSERT_NE(CmpIt, states.front()->values.end());
  ASSERT_NE(QIt, states.front()->values.end());
  ASSERT_NE(SIt, states.front()->values.end());

  expectConstValue(ZIt->second, unsignedAPInt(32, 1));
  expectConstValue(TIt->second, signedAPInt(8, 0));
  expectConstValue(CmpIt->second, unsignedAPInt(1, 1));
  expectConstValue(QIt->second, unsignedAPInt(32, 2147483647));
  expectConstValue(SIt->second, unsignedAPInt(32, 2147483647));
}

TEST(NPAInterproceduralClients,
     ConstantPropagationSupportsLargeUnsignedValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %q = udiv i64 -1, 1
      %s = lshr i64 -1, 0
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Q = findInstructionByName(*Main, "q");
  auto *S = findInstructionByName(*Main, "s");
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto QIt = states.front()->values.find(Q);
  auto SIt = states.front()->values.find(S);
  ASSERT_NE(QIt, states.front()->values.end());
  ASSERT_NE(SIt, states.front()->values.end());
  expectConstValue(QIt->second,
                   unsignedAPInt(64, std::numeric_limits<uint64_t>::max()));
  expectConstValue(SIt->second,
                   unsignedAPInt(64, std::numeric_limits<uint64_t>::max()));
}

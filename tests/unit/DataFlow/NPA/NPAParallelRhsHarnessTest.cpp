#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralIntervalAnalysis.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralLiveVariables.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralMaybeUninitialized.h"
#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralRD.h"
#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/NPA.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

struct BoolSemiring {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = true;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type a, value_type b) { return a == b; }
  static value_type combine(value_type a, value_type b) { return a || b; }
  static value_type extend(value_type a, value_type b) { return a && b; }
  static value_type extend_lin(value_type a, value_type b) {
    return extend(a, b);
  }
  static value_type ndetCombine(value_type a, value_type b) {
    return combine(a, b);
  }
  static value_type condCombine(test_type phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type subtract(value_type a, value_type b) { return a && !b; }
};

struct LimitedBoolSemiring : BoolSemiring {
  static constexpr long max_linear_steps = 1;
};

struct LimitedFixpointBoolSemiring : BoolSemiring {
  static constexpr int max_fixpoint_iters = 0;
};

template <class D>
std::unordered_map<npa::Symbol, npa::DomVal<D>>
toMap(const std::vector<std::pair<npa::Symbol, npa::DomVal<D>>> &pairs) {
  std::unordered_map<npa::Symbol, npa::DomVal<D>> out;
  for (const auto &pair : pairs)
    out.emplace(pair.first, pair.second);
  return out;
}

template <typename T>
std::vector<const T *> statesForBlock(const std::map<npa::BlockKey, T> &facts,
                                      const llvm::BasicBlock *block) {
  std::vector<const T *> out;
  for (const auto &entry : facts) {
    if (entry.first.block == block)
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

template <typename T, typename Eq>
void expectBlockFactsEqual(const std::map<npa::BlockKey, T> &lhs,
                           const std::map<npa::BlockKey, T> &rhs, Eq eq) {
  ASSERT_EQ(lhs.size(), rhs.size());
  auto lhs_it = lhs.begin();
  auto rhs_it = rhs.begin();
  for (; lhs_it != lhs.end(); ++lhs_it, ++rhs_it) {
    EXPECT_EQ(lhs_it->first.block, rhs_it->first.block);
    EXPECT_TRUE(eq(lhs_it->second, rhs_it->second));
  }
}

template <typename T, typename Eq>
void expectSummariesEqual(const std::map<npa::FunctionKey, T> &lhs,
                          const std::map<npa::FunctionKey, T> &rhs, Eq eq) {
  ASSERT_EQ(lhs.size(), rhs.size());
  auto lhs_it = lhs.begin();
  auto rhs_it = rhs.begin();
  for (; lhs_it != lhs.end(); ++lhs_it, ++rhs_it) {
    EXPECT_EQ(lhs_it->first.function, rhs_it->first.function);
    EXPECT_TRUE(eq(lhs_it->second, rhs_it->second));
  }
}

void expectAnalysisStatusEquivalent(const npa::AnalysisStatus &lhs,
                                    const npa::AnalysisStatus &rhs) {
  EXPECT_EQ(lhs.summary_solve.converged, rhs.summary_solve.converged);
  EXPECT_EQ(lhs.summary_solve.hit_limit, rhs.summary_solve.hit_limit);
  EXPECT_EQ(lhs.summary_solve.hit_outer_limit, rhs.summary_solve.hit_outer_limit);
  EXPECT_EQ(lhs.summary_solve.hit_linear_limit,
            rhs.summary_solve.hit_linear_limit);
  EXPECT_EQ(lhs.summary_solve.hit_fixpoint_limit,
            rhs.summary_solve.hit_fixpoint_limit);
  EXPECT_EQ(lhs.summary_solve.equation_count, rhs.summary_solve.equation_count);
  EXPECT_EQ(lhs.summary_solve.requested_max_iters,
            rhs.summary_solve.requested_max_iters);
  EXPECT_EQ(lhs.summary_solve.effective_max_iters,
            rhs.summary_solve.effective_max_iters);
  EXPECT_EQ(lhs.summary_solve.used_approx_equal,
            rhs.summary_solve.used_approx_equal);
  EXPECT_EQ(lhs.summary_solve.used_auto_n_cap,
            rhs.summary_solve.used_auto_n_cap);
  EXPECT_EQ(lhs.summary_solve.retried_without_auto_n_cap,
            rhs.summary_solve.retried_without_auto_n_cap);
  EXPECT_EQ(lhs.summary_solve.adaptive_scc_used,
            rhs.summary_solve.adaptive_scc_used);
  EXPECT_EQ(lhs.summary_solve.adaptive_scc_direct_count,
            rhs.summary_solve.adaptive_scc_direct_count);
  EXPECT_EQ(lhs.summary_solve.adaptive_scc_worklist_count,
            rhs.summary_solve.adaptive_scc_worklist_count);
  EXPECT_EQ(lhs.summary_solve.adaptive_scc_tensor_count,
            rhs.summary_solve.adaptive_scc_tensor_count);
  EXPECT_EQ(lhs.summary_solve.adaptive_scc_tensor_fallback_count,
            rhs.summary_solve.adaptive_scc_tensor_fallback_count);
  EXPECT_EQ(lhs.propagation_steps, rhs.propagation_steps);
  EXPECT_EQ(lhs.propagation_converged, rhs.propagation_converged);
  EXPECT_EQ(lhs.propagation_hit_limit, rhs.propagation_hit_limit);
  EXPECT_EQ(lhs.configuration_error, rhs.configuration_error);
  EXPECT_EQ(lhs.unsupported_specs, rhs.unsupported_specs);
  EXPECT_EQ(lhs.approximated, rhs.approximated);
  EXPECT_EQ(lhs.used_summary_overflow, rhs.used_summary_overflow);
  EXPECT_EQ(lhs.used_fact_widening, rhs.used_fact_widening);
  EXPECT_EQ(lhs.used_bounded_inner_solve, rhs.used_bounded_inner_solve);
  EXPECT_EQ(lhs.overall_converged, rhs.overall_converged);
  EXPECT_EQ(lhs.overall_hit_limit, rhs.overall_hit_limit);
  EXPECT_EQ(lhs.call_resolution_mode, rhs.call_resolution_mode);
  EXPECT_EQ(lhs.indirect_calls_seen, rhs.indirect_calls_seen);
  EXPECT_EQ(lhs.unresolved_indirect_calls, rhs.unresolved_indirect_calls);
  EXPECT_EQ(lhs.fallback_call_edges, rhs.fallback_call_edges);
  EXPECT_EQ(lhs.requires_external_callee_resolver,
            rhs.requires_external_callee_resolver);
  EXPECT_EQ(lhs.open_world_unsound_mode, rhs.open_world_unsound_mode);
}

llvm::APInt signedAPInt(unsigned bit_width, int64_t value) {
  return llvm::APInt(bit_width, static_cast<uint64_t>(value), true);
}

void expectConstValue(const npa::ConstantPropagationValue &value,
                      const llvm::APInt &expected) {
  EXPECT_EQ(value.tag, npa::ConstantPropagationTag::Const);
  EXPECT_EQ(value.constant.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.constant.eq(expected));
}

void expectIntervalPoint(const npa::Interval &value, const llvm::APInt &expected,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  EXPECT_TRUE(value.lower.eq(expected));
  EXPECT_TRUE(value.upper.eq(expected));
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> sortedPredicateTransitions(
    const npa::PredicateRelationDomain::value_type &relation) {
  auto transitions = npa::PredicateRelationDomain::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

std::string buildScalarIdentityChainIR(unsigned depth) {
  std::ostringstream ir;
  for (unsigned i = 0; i < depth; ++i) {
    ir << "define i32 @f" << i << "(i32 %x) {\n";
    ir << "entry:\n";
    if (i + 1 == depth) {
      ir << "  ret i32 %x\n";
    } else {
      ir << "  %r = call i32 @f" << (i + 1) << "(i32 %x)\n";
      ir << "  ret i32 %r\n";
    }
    ir << "}\n\n";
  }
  ir << "define i32 @main() {\n";
  ir << "entry:\n";
  ir << "  %seed = add i32 0, 5\n";
  ir << "  %r = call i32 @f0(i32 %seed)\n";
  ir << "  ret i32 %r\n";
  ir << "}\n";
  return ir.str();
}

std::string buildPointerIdentityChainIR(unsigned depth) {
  std::ostringstream ir;
  for (unsigned i = 0; i < depth; ++i) {
    ir << "define i32 @g" << i << "(i32* %p) {\n";
    ir << "entry:\n";
    if (i + 1 == depth) {
      ir << "  %v = load i32, i32* %p\n";
      ir << "  ret i32 %v\n";
    } else {
      ir << "  %r = call i32 @g" << (i + 1) << "(i32* %p)\n";
      ir << "  ret i32 %r\n";
    }
    ir << "}\n\n";
  }
  ir << "define i32 @main() {\n";
  ir << "entry:\n";
  ir << "  %p = alloca i32\n";
  ir << "  %r = call i32 @g0(i32* %p)\n";
  ir << "  ret i32 %r\n";
  ir << "}\n";
  return ir.str();
}

const unsigned kSafeCoreChainDepth =
    static_cast<unsigned>(npa::detail::newton_parallel_setup_min_equations()) +
    1U;

} // namespace

TEST(NPAParallelRhsHarness, NewtonInitMatchesAcrossSetupModes) {
  using D = BoolSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("x", Exp0::hole("y"));
  eqns.emplace_back("y", Exp0::term(D::one()));

  auto serial = npa::detail::build_newton_initial_values<
      D>(eqns, npa::detail::NewtonSetupExecutionMode::ForceSerial);
  auto parallel = npa::detail::build_newton_initial_values<
      D>(eqns, npa::detail::NewtonSetupExecutionMode::ForceParallel);

  EXPECT_EQ(toMap<D>(serial), toMap<D>(parallel));
}

TEST(NPAParallelRhsHarness, NewtonRunMatchesAcrossSetupModes) {
  using D = BoolSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("x", Exp0::hole("y"));
  eqns.emplace_back("y", Exp0::term(D::one()));

  auto binds = npa::detail::build_newton_initial_values<
      D>(eqns, npa::detail::NewtonSetupExecutionMode::ForceSerial);
  auto serial = npa::detail::run_newton_iteration<
      D>(false, eqns, binds, npa::LinearStrategy::SCC,
         npa::detail::NewtonSetupExecutionMode::ForceSerial);
  auto parallel = npa::detail::run_newton_iteration<
      D>(false, eqns, binds, npa::LinearStrategy::SCC,
         npa::detail::NewtonSetupExecutionMode::ForceParallel);

  EXPECT_EQ(toMap<D>(serial), toMap<D>(parallel));
}

TEST(NPAParallelRhsHarness, NewtonSolverPropagatesFixpointLimitFromSetupTasks) {
  using D = LimitedFixpointBoolSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  for (unsigned i = 0; i < kSafeCoreChainDepth; ++i) {
    eqns.emplace_back(
        "X" + std::to_string(i),
        Exp0::star(Exp0::ndet(Exp0::bound("b"), Exp0::term(D::one())), "b"));
  }

  auto result = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                            npa::LinearStrategy::SCC);
  auto solved = toMap<D>(result.first);

  EXPECT_FALSE(result.second.converged);
  EXPECT_TRUE(result.second.hit_limit);
  EXPECT_FALSE(result.second.hit_outer_limit);
  EXPECT_FALSE(result.second.hit_linear_limit);
  EXPECT_TRUE(result.second.hit_fixpoint_limit);
  for (const auto &entry : solved)
    EXPECT_TRUE(entry.second);
}

TEST(NPAParallelRhsHarness, VerboseModeDisablesAutomaticParallelSetup) {
  const auto min_eqns = npa::detail::newton_parallel_setup_min_equations();
  EXPECT_FALSE(npa::detail::should_parallelize_newton_setup(
      true, min_eqns, npa::detail::NewtonSetupExecutionMode::Auto));
  EXPECT_EQ(npa::detail::should_parallelize_newton_setup(
                false, min_eqns, npa::detail::NewtonSetupExecutionMode::Auto),
            ThreadPool::get()->hasWorkers());
}

TEST(NPAParallelRhsHarness, TensorNewtonSetupMatchesAcrossSetupModes) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);

  auto binds = npa::detail::build_newton_initial_values<
      D>(eqns, npa::detail::NewtonSetupExecutionMode::ForceSerial);
  auto serial = npa::detail::run_newton_iteration<
      D>(false, eqns, binds, npa::LinearStrategy::TensorProduct,
         npa::detail::NewtonSetupExecutionMode::ForceSerial);
  auto parallel = npa::detail::run_newton_iteration<
      D>(false, eqns, binds, npa::LinearStrategy::TensorProduct,
         npa::detail::NewtonSetupExecutionMode::ForceParallel);

  ASSERT_EQ(serial.size(), 1u);
  ASSERT_EQ(parallel.size(), 1u);
  EXPECT_TRUE(D::equal(serial[0].second, parallel[0].second));
}

TEST(NPAParallelRhsHarness, SccPlanCapturesIndependentParallelLayer) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("X", Exp1::term(true));
  rhs.emplace_back("Y", Exp1::term(true));
  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());

  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  ASSERT_EQ(plan.layers.size(), 1u);
  ASSERT_EQ(plan.layers.front().size(), 2u);
  EXPECT_TRUE(plan.has_nontrivial_parallelism);
  EXPECT_EQ(npa::detail::should_parallelize_linear_scc(false, plan),
            ThreadPool::get()->hasWorkers());

  auto serial =
      npa::detail::solve_linear_scc_serial_from_plan<D>(false, rhs, init, plan);
  auto parallel = npa::detail::solve_linear_scc_parallel_from_plan<D>(
      false, rhs, init, plan);

  EXPECT_EQ(serial, parallel);
  ASSERT_EQ(serial.size(), 2u);
  EXPECT_TRUE(serial[0]);
  EXPECT_TRUE(serial[1]);
}

TEST(NPAParallelRhsHarness, SccPlanHandlesLayeredDependencies) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("A", Exp1::term(true));
  rhs.emplace_back("B", Exp1::term(true));
  rhs.emplace_back("C", Exp1::hole("A"));
  rhs.emplace_back("D", Exp1::hole("B"));
  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());

  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  ASSERT_EQ(plan.layers.size(), 2u);
  EXPECT_EQ(plan.layers[0].size(), 2u);
  EXPECT_EQ(plan.layers[1].size(), 2u);

  auto serial =
      npa::detail::solve_linear_scc_serial_from_plan<D>(false, rhs, init, plan);
  auto parallel = npa::detail::solve_linear_scc_parallel_from_plan<D>(
      false, rhs, init, plan);

  EXPECT_EQ(serial, parallel);
  ASSERT_EQ(serial.size(), 4u);
  EXPECT_TRUE(serial[0]);
  EXPECT_TRUE(serial[1]);
  EXPECT_TRUE(serial[2]);
  EXPECT_TRUE(serial[3]);
}

TEST(NPAParallelRhsHarness, SccPlanHandlesSingleCyclicComponent) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("X", Exp1::hole("Y"));
  rhs.emplace_back("Y", Exp1::add(Exp1::term(true), Exp1::hole("X")));
  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());

  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  ASSERT_EQ(plan.layers.size(), 1u);
  ASSERT_EQ(plan.layers.front().size(), 1u);
  EXPECT_FALSE(plan.has_nontrivial_parallelism);

  auto serial =
      npa::detail::solve_linear_scc_serial_from_plan<D>(false, rhs, init, plan);
  auto parallel = npa::detail::solve_linear_scc_parallel_from_plan<D>(
      false, rhs, init, plan);

  EXPECT_EQ(serial, parallel);
  ASSERT_EQ(serial.size(), 2u);
  EXPECT_TRUE(serial[0]);
  EXPECT_TRUE(serial[1]);
}

TEST(NPAParallelRhsHarness, SccParallelPathPreservesExactLinearLimit) {
  using D = LimitedBoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("X", Exp1::hole("Y"));
  rhs.emplace_back("Y", Exp1::add(Exp1::term(true), Exp1::hole("X")));
  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());

  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);

  npa::npa_reset_limit_hit();
  auto serial =
      npa::detail::solve_linear_scc_serial_from_plan<D>(false, rhs, init, plan);
  EXPECT_TRUE(npa::npa_hit_linear_limit());

  npa::npa_reset_limit_hit();
  auto parallel = npa::detail::solve_linear_scc_parallel_from_plan<D>(
      false, rhs, init, plan);
  EXPECT_TRUE(npa::npa_hit_linear_limit());
  EXPECT_EQ(serial, parallel);
}

TEST(NPAParallelRhsHarness, VerboseSccModeForcesSerialScheduling) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("X", Exp1::term(true));
  rhs.emplace_back("Y", Exp1::term(true));
  std::vector<npa::DomVal<D>> init(rhs.size(), D::zero());

  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  EXPECT_FALSE(npa::detail::should_parallelize_linear_scc(true, plan));

  testing::internal::CaptureStderr();
  auto result = npa::solve_linear_scc_impl<D>(true, rhs, init);
  std::string stderr_output = testing::internal::GetCapturedStderr();

  EXPECT_EQ(result, (std::vector<npa::DomVal<D>>{true, true}));
  EXPECT_NE(stderr_output.find("[linear-scc] steps="), std::string::npos);
}

TEST(NPAParallelRhsHarness,
     MaybeUninitializedChainProducesExpectedFactsAcrossWorkerCounts) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildPointerIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto *Deep = module->getFunction(
      ("g" + std::to_string(kSafeCoreChainDepth - 1)).c_str());
  ASSERT_NE(Deep, nullptr);

  auto worklist = npa::InterproceduralMaybeUninitialized::run(
      *module, false, npa::LinearStrategy::SCC);
  auto scc = npa::InterproceduralMaybeUninitialized::run(
      *module, false, npa::LinearStrategy::SCC);
  llvm::APInt entry_fact = unionFactForBlock(scc.blockFacts, &Deep->getEntryBlock());
  EXPECT_GT(entry_fact.countPopulation(), 0u);
  expectSummariesEqual(worklist.summaries, scc.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return npa::TaintTransferDomain::equal(lhs, rhs);
                       });
  expectBlockFactsEqual(worklist.blockFacts, scc.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  expectAnalysisStatusEquivalent(worklist.status, scc.status);
  EXPECT_TRUE(scc.status.summary_solve.converged);
}

TEST(NPAParallelRhsHarness,
     ReachingDefinitionsChainProducesExpectedFactsAcrossWorkerCounts) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto *Deep = module->getFunction(
      ("f" + std::to_string(kSafeCoreChainDepth - 1)).c_str());
  ASSERT_NE(Deep, nullptr);

  auto worklist = npa::InterproceduralRD::run(
      *module, false, npa::LinearStrategy::SCC);
  auto scc = npa::InterproceduralRD::run(*module, false,
                                         npa::LinearStrategy::SCC);
  llvm::APInt entry_fact = unionFactForBlock(scc.blockFacts, &Deep->getEntryBlock());
  EXPECT_GT(entry_fact.countPopulation(), 0u);
  expectSummariesEqual(worklist.summaries, scc.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return lhs == rhs;
                       });
  expectBlockFactsEqual(worklist.blockFacts, scc.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  expectAnalysisStatusEquivalent(worklist.status, scc.status);
  EXPECT_TRUE(scc.status.summary_solve.converged);
}

TEST(NPAParallelRhsHarness,
     LiveVariablesChainProducesExpectedFactsAcrossWorkerCounts) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto *Deep = module->getFunction(
      ("f" + std::to_string(kSafeCoreChainDepth - 1)).c_str());
  ASSERT_NE(Deep, nullptr);
  auto *Arg = &*Deep->arg_begin();

  auto worklist = npa::InterproceduralLiveVariables::run(
      *module, false, npa::LinearStrategy::SCC);
  auto scc = npa::InterproceduralLiveVariables::run(*module, false,
                                                    npa::LinearStrategy::SCC);
  auto bit_it = scc.valueBits.find(Arg);
  ASSERT_NE(bit_it, scc.valueBits.end());

  llvm::APInt live_in = unionFactForBlock(scc.blockFacts, &Deep->getEntryBlock());
  EXPECT_TRUE(live_in[bit_it->second]);
  expectSummariesEqual(worklist.summaries, scc.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return npa::TaintTransferDomain::equal(lhs, rhs);
                       });
  expectBlockFactsEqual(worklist.blockFacts, scc.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  EXPECT_EQ(worklist.valueBits, scc.valueBits);
  EXPECT_EQ(worklist.bitWidth, scc.bitWidth);
  expectAnalysisStatusEquivalent(worklist.status, scc.status);
  EXPECT_TRUE(scc.status.summary_solve.converged);
}

TEST(NPAParallelRhsHarness,
     ConstantPropagationChainProducesExpectedFactsAcrossWorkerCounts) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto *Deep = module->getFunction(
      ("f" + std::to_string(kSafeCoreChainDepth - 1)).c_str());
  ASSERT_NE(Deep, nullptr);
  auto *Arg = &*Deep->arg_begin();

  auto worklist = npa::InterproceduralConstantPropagation::run(
      *module, false, npa::LinearStrategy::SCC);
  auto scc = npa::InterproceduralConstantPropagation::run(
      *module, false, npa::LinearStrategy::SCC);
  auto states = statesForBlock(scc.blockFacts, &Deep->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto it = states.front()->values.find(Arg);
  ASSERT_NE(it, states.front()->values.end());
  expectConstValue(it->second, signedAPInt(32, 5));
  expectSummariesEqual(worklist.summaries, scc.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return lhs == rhs;
                       });
  expectBlockFactsEqual(worklist.blockFacts, scc.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  expectAnalysisStatusEquivalent(worklist.status, scc.status);
  EXPECT_TRUE(scc.status.summary_solve.converged);
  EXPECT_TRUE(scc.status.overall_converged);
}

TEST(NPAParallelRhsHarness,
     IntervalChainProducesExpectedFactsAcrossWorkerCounts) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto *Deep = module->getFunction(
      ("f" + std::to_string(kSafeCoreChainDepth - 1)).c_str());
  ASSERT_NE(Deep, nullptr);
  auto *Arg = &*Deep->arg_begin();

  auto worklist = npa::InterproceduralIntervalAnalysis::run(
      *module, false, npa::LinearStrategy::SCC);
  auto scc = npa::InterproceduralIntervalAnalysis::run(
      *module, false, npa::LinearStrategy::SCC);
  auto states = statesForBlock(scc.blockFacts, &Deep->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto it = states.front()->values.find(Arg);
  ASSERT_NE(it, states.front()->values.end());
  expectIntervalPoint(it->second, signedAPInt(32, 5),
                      npa::IntervalOrdering::Signed);
  expectSummariesEqual(worklist.summaries, scc.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return lhs == rhs;
                       });
  expectBlockFactsEqual(worklist.blockFacts, scc.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  expectAnalysisStatusEquivalent(worklist.status, scc.status);
  EXPECT_TRUE(scc.status.summary_solve.converged);
  EXPECT_TRUE(scc.status.overall_converged);
}

TEST(NPAParallelRhsHarness,
     ConstantPropagationTensorFallbackPreservesResultsAcrossWorkerCounts) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto *Deep = module->getFunction(
      ("f" + std::to_string(kSafeCoreChainDepth - 1)).c_str());
  ASSERT_NE(Deep, nullptr);
  auto *Arg = &*Deep->arg_begin();

  auto worklist =
      npa::InterproceduralConstantPropagation::run(*module, false,
                                                   npa::LinearStrategy::SCC);
  auto tensor = npa::InterproceduralConstantPropagation::run(
      *module, false, npa::LinearStrategy::TensorProduct);

  auto worklist_states = statesForBlock(worklist.blockFacts, &Deep->getEntryBlock());
  auto tensor_states = statesForBlock(tensor.blockFacts, &Deep->getEntryBlock());
  ASSERT_EQ(worklist_states.size(), 1u);
  ASSERT_EQ(tensor_states.size(), 1u);
  auto wl_it = worklist_states.front()->values.find(Arg);
  auto tp_it = tensor_states.front()->values.find(Arg);
  ASSERT_NE(wl_it, worklist_states.front()->values.end());
  ASSERT_NE(tp_it, tensor_states.front()->values.end());
  expectConstValue(wl_it->second, signedAPInt(32, 5));
  expectConstValue(tp_it->second, signedAPInt(32, 5));
  EXPECT_EQ(worklist.status.summary_solve.converged,
            tensor.status.summary_solve.converged);
  EXPECT_EQ(worklist.status.overall_converged, tensor.status.overall_converged);
}

TEST(NPAParallelRhsHarness,
     ConstantPropagationAdaptiveSccMatchesSccAndReportsStats) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto scc = npa::InterproceduralConstantPropagation::run(
      *module, false, npa::LinearStrategy::SCC);
  auto adaptive = npa::InterproceduralConstantPropagation::run(
      *module, false, npa::LinearStrategy::AdaptiveScc);

  expectSummariesEqual(scc.summaries, adaptive.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return lhs == rhs;
                       });
  expectBlockFactsEqual(scc.blockFacts, adaptive.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  EXPECT_TRUE(adaptive.status.summary_solve.adaptive_scc_used);
  EXPECT_GE(adaptive.status.summary_solve.adaptive_scc_direct_count, 1);
}

TEST(NPAParallelRhsHarness, IntervalAdaptiveSccMatchesSccAndReportsStats) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, buildScalarIdentityChainIR(kSafeCoreChainDepth));
  ASSERT_NE(module, nullptr);

  auto scc = npa::InterproceduralIntervalAnalysis::run(
      *module, false, npa::LinearStrategy::SCC);
  auto adaptive = npa::InterproceduralIntervalAnalysis::run(
      *module, false, npa::LinearStrategy::AdaptiveScc);

  expectSummariesEqual(scc.summaries, adaptive.summaries,
                       [](const auto &lhs, const auto &rhs) {
                         return lhs == rhs;
                       });
  expectBlockFactsEqual(scc.blockFacts, adaptive.blockFacts,
                        [](const auto &lhs, const auto &rhs) {
                          return lhs == rhs;
                        });
  EXPECT_TRUE(adaptive.status.summary_solve.adaptive_scc_used);
  EXPECT_GE(adaptive.status.summary_solve.adaptive_scc_direct_count, 1);
}

TEST(NPAParallelRhsHarness,
     PredicateTensorSolverProducesDeterministicSummaryAcrossWorkerCounts) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);

  auto worklist = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                              npa::LinearStrategy::SCC);
  auto tensor = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                            npa::LinearStrategy::TensorProduct);

  ASSERT_EQ(worklist.first.size(), 1u);
  ASSERT_EQ(tensor.first.size(), 1u);
  EXPECT_TRUE(D::equal(worklist.first[0].second, tensor.first[0].second));
  EXPECT_EQ(sortedPredicateTransitions(tensor.first[0].second),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 0}, {0, 1}, {1, 1}, {2, 2}, {2, 3}, {3, 3}}));
}

TEST(NPAParallelRhsHarness,
     TensorProductStrategyBypassesSccParallelismAndPreservesResults) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);

  auto tensor = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                            npa::LinearStrategy::TensorProduct);
  auto scc = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                         npa::LinearStrategy::SCC);

  ASSERT_EQ(tensor.first.size(), 1u);
  ASSERT_EQ(scc.first.size(), 1u);
  EXPECT_TRUE(D::equal(tensor.first[0].second, scc.first[0].second));
}

TEST(NPAParallelRhsHarness, AdaptiveSccPlanChoosesDirectForAcyclicSingletons) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("A", Exp1::term(true));
  rhs.emplace_back("B", Exp1::hole("A"));

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  npa::detail::NewtonRoundSetup<D> setup;
  npa::detail::annotate_adaptive_scc_plan<D>(plan, rhs, rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 2u);
  for (const auto &info : plan.infos) {
    EXPECT_EQ(info.strategy, npa::detail::SccStrategy::Direct);
    EXPECT_FALSE(info.is_cyclic);
    EXPECT_FALSE(info.tensor_fallback);
  }
}

TEST(NPAParallelRhsHarness, AdaptiveSccPlanChoosesWorklistForNonLcflCycle) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back("X", Exp1::hole("Y"));
  rhs.emplace_back("Y", Exp1::add(Exp1::term(true), Exp1::hole("X")));

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  npa::detail::NewtonRoundSetup<D> setup;
  npa::detail::annotate_adaptive_scc_plan<D>(plan, rhs, rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 1u);
  EXPECT_EQ(plan.infos.front().strategy, npa::detail::SccStrategy::Worklist);
  EXPECT_TRUE(plan.infos.front().is_cyclic);
  EXPECT_FALSE(plan.infos.front().has_lcfl_structure);
  EXPECT_FALSE(plan.infos.front().tensor_fallback);
}

TEST(NPAParallelRhsHarness, AdaptiveSccPlanChoosesTensorForLcflCycle) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);
  auto binds = npa::detail::build_newton_initial_values<D>(eqns);
  auto setup = npa::detail::build_newton_round_setup<D>(
      false, eqns, binds, npa::LinearStrategy::AdaptiveScc);
  auto plan = npa::detail::build_linear_scc_plan<D>(setup.rhs);
  npa::detail::annotate_adaptive_scc_plan<D>(plan, setup.rhs, setup.rhs_tensor,
                                             setup);

  ASSERT_EQ(plan.infos.size(), 1u);
  EXPECT_EQ(plan.infos.front().strategy, npa::detail::SccStrategy::Tensor);
  EXPECT_TRUE(plan.infos.front().is_cyclic);
  EXPECT_TRUE(plan.infos.front().has_lcfl_structure);
  EXPECT_TRUE(plan.infos.front().tensor_eligible);
}

TEST(NPAParallelRhsHarness, AdaptiveSccPlanTracksTensorFallbackWhenUnavailable) {
  using D = BoolSemiring;
  using Exp1 = npa::Exp1<D>;
  using E1 = npa::E1<D>;
  using TD = typename npa::TensorSemiringTraits<D>::tensor_domain;

  std::vector<std::pair<npa::Symbol, E1>> rhs;
  rhs.emplace_back(
      "X", Exp1::add(Exp1::term(true),
                     Exp1::concat(Exp1::term(true), "X", Exp1::term(true))));

  std::vector<std::pair<npa::Symbol, npa::E1<TD>>> rhs_tensor;
  auto plan = npa::detail::build_linear_scc_plan<D>(rhs);
  npa::detail::NewtonRoundSetup<D> setup;
  npa::detail::annotate_adaptive_scc_plan<D>(plan, rhs, rhs_tensor, setup);

  ASSERT_EQ(plan.infos.size(), 1u);
  EXPECT_EQ(plan.infos.front().strategy, npa::detail::SccStrategy::Worklist);
  EXPECT_TRUE(plan.infos.front().has_lcfl_structure);
  EXPECT_TRUE(plan.infos.front().tensor_fallback);
  EXPECT_EQ(plan.infos.front().tensor_fallback_reason,
            npa::detail::TensorFallbackReason::TensorUnavailable);
}

TEST(NPAParallelRhsHarness,
     AdaptiveSccMatchesSccOnMixedOrdinarySystemAndReportsCounts) {
  using D = BoolSemiring;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("A", Exp0::term(true));
  eqns.emplace_back("X", Exp0::hole("Y"));
  eqns.emplace_back("Y", Exp0::ndet(Exp0::term(true), Exp0::hole("X")));

  auto scc =
      npa::NewtonSolver<D>::solve(eqns, false, -1, npa::LinearStrategy::SCC);
  auto adaptive = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                              npa::LinearStrategy::AdaptiveScc);

  EXPECT_EQ(toMap<D>(scc.first), toMap<D>(adaptive.first));
  EXPECT_TRUE(adaptive.second.converged);
  EXPECT_TRUE(adaptive.second.adaptive_scc_used);
  EXPECT_GE(adaptive.second.adaptive_scc_direct_count, 1);
  EXPECT_GE(adaptive.second.adaptive_scc_worklist_count, 1);
  EXPECT_EQ(adaptive.second.adaptive_scc_tensor_count, 0);
  EXPECT_EQ(adaptive.second.adaptive_scc_tensor_fallback_count, 0);
}

TEST(NPAParallelRhsHarness,
     AdaptiveSccMatchesTensorOnTensorEligibleSystemAndReportsCounts) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("X", rhs);

  auto tensor = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                            npa::LinearStrategy::TensorProduct);
  auto adaptive = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                              npa::LinearStrategy::AdaptiveScc);

  ASSERT_EQ(tensor.first.size(), 1u);
  ASSERT_EQ(adaptive.first.size(), 1u);
  EXPECT_TRUE(D::equal(tensor.first[0].second, adaptive.first[0].second));
  EXPECT_TRUE(adaptive.second.adaptive_scc_used);
  EXPECT_EQ(adaptive.second.adaptive_scc_direct_count, 0);
  EXPECT_EQ(adaptive.second.adaptive_scc_worklist_count, 0);
  EXPECT_GE(adaptive.second.adaptive_scc_tensor_count, 1);
  EXPECT_EQ(adaptive.second.adaptive_scc_tensor_fallback_count, 0);
}

TEST(NPAParallelRhsHarness,
     AdaptiveSccMatchesSccOnMixedTensorAndDirectSystemAndReportsCounts) {
  using D = npa::PredicateRelationDomain;
  using E0 = npa::E0<D>;
  using Exp0 = npa::Exp0<D>;

  D::configure(2, 1);
  E0 set_global_true = Exp0::term(D::assignConst(0, true));
  E0 set_local_true = Exp0::term(D::assignConst(1, true));
  E0 id = Exp0::term(D::one());
  E0 tensor_rhs = Exp0::project(
      Exp0::ndet(id, Exp0::concat(set_global_true, "X", set_local_true)));

  std::vector<std::pair<npa::Symbol, E0>> eqns;
  eqns.emplace_back("A", id);
  eqns.emplace_back("X", tensor_rhs);

  auto scc =
      npa::NewtonSolver<D>::solve(eqns, false, -1, npa::LinearStrategy::SCC);
  auto adaptive = npa::NewtonSolver<D>::solve(eqns, false, -1,
                                              npa::LinearStrategy::AdaptiveScc);

  ASSERT_EQ(scc.first.size(), adaptive.first.size());
  for (std::size_t i = 0; i < scc.first.size(); ++i)
    EXPECT_TRUE(D::equal(scc.first[i].second, adaptive.first[i].second));
  EXPECT_GE(adaptive.second.adaptive_scc_direct_count, 1);
  EXPECT_GE(adaptive.second.adaptive_scc_tensor_count, 1);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "NPA parallel RHS harness\n");
  return RUN_ALL_TESTS();
}

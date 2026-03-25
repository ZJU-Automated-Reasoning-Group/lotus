/**
 * @file UnilateralAnalyzer.cpp
 * @brief Unilateral (forward) analyzer implementation with incremental SMT
 solving. See Algorithm 6 in: Thakur, A. V. (2014, August). Symbolic
 Abstraction: Algorithms and Applications (Ph.D. dissertation). Computer
 Sciences Department, University of Wisconsin, Madison.

 * Author: rainoftime
 */
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <cstdlib>
#include <iostream>

namespace symabs_ai {
/**
 * Computes the best transformer for a fragment using a unilateral (forward)
 * abstract interpretation scheme.
 *
 * The method optionally reuses an incremental SMT solver per fragment:
 * it caches the fragment's semantic formula and then, for each distinct
 * input abstract value, adds a guarded copy of its formula under a fresh
 * indicator variable. This allows multiple calls with different inputs
 * to share solver state while keeping them logically separated via
 * assumptions.
 */
bool UnilateralAnalyzer::bestTransformer(const AbstractValue *input,
                                         const Fragment &fragment,
                                         AbstractValue *result) const {
  Analyzer::countBestTransformerCall();
  if (std::getenv("LOTUS_TRACE_BEST_TRANSFORMER")) {
    std::cerr << "  [UnilateralAnalyzer::bestTransformer entered #"
              << Analyzer::getBestTransformerCallCount() << "]\n";
  }
  VOutBlock vout_block("best transformer for " + repr(fragment));
  CurrentFragment_ = &fragment;
  z3::context &ctx = FunctionContext_.getZ3();
  bool incremental =
      FunctionContext_.getConfig().get<bool>("Analyzer", "Incremental", true);
  std::vector<z3::expr> assumptions;

  {
    VOutBlock vb("input");
    vout << *input;
  }

  // find an appropriate cache entry or create a new one
  unique_ptr<TransfCacheData> temp_entry; // lives through whole function
  TransfCacheData *cache_entry;
  if (incremental && TransfCache_.find(&fragment) != TransfCache_.end()) {
    cache_entry = TransfCache_[&fragment].get();
  } else {
    // Determine whether it's a temporary fragment (in which case we must
    // not store a cache entry for it). If incremental SMT is disabled, we
    // treat everything like a temporary fragment.
    bool temp_frag = true;
    if (incremental) {
      for (auto &frag : Fragments_) {
        if (&frag == &fragment)
          temp_frag = false;
      }
    }

    // allocate either persistent cache entry or temporary entry
    if (temp_frag) {
      temp_entry.reset(new TransfCacheData(ctx));
      cache_entry = temp_entry.get();
    } else {
      auto &up_ref = TransfCache_[&fragment];
      up_ref.reset(new TransfCacheData(ctx));
      cache_entry = up_ref.get();
    }

    // initialize the entry
    cache_entry->solver.add(FunctionContext_.formulaFor(fragment));
  }

  // generate the formula for the input abstract value
  ValueMapping vm_before =
      ValueMapping::atBeginning(FunctionContext_, fragment);
  z3::expr av_formula = input->toFormula(vm_before, ctx);

  if (incremental) {
    // fill the assumption vector with old indicator input variables
    for (auto &ind_var : cache_entry->ind_vars)
      assumptions.push_back(!ind_var);

    // create a fresh indication variable and construct the input formula
    std::ostringstream ind_var_name;
    ind_var_name << INPUT_VAR_PREFIX << cache_entry->ind_vars.size();
    z3::expr ind_var = ctx.bool_const(ind_var_name.str().c_str());
    cache_entry->solver.add(ind_var == av_formula);
    assumptions.push_back(ind_var);
    cache_entry->ind_vars.push_back(ind_var);
  } else {
    // non-incremental case: don't bother with indicator variables
    cache_entry->solver.add(av_formula);
  }

  ValueMapping vm_after = ValueMapping::atEnd(FunctionContext_, fragment);
  bool res = strongestConsequence(result, vm_after, &cache_entry->solver,
                                  &assumptions);

  {
    VOutBlock vb("result");
    vout << *result;
  }

  CurrentFragment_ = nullptr;
  return res;
}

/**
 * Model-enumeration loop for computing the strongest abstract consequence.
 *
 * Starting from the current abstract value `result`, repeatedly ask the
 * solver for a model that violates `result` (by asserting ¬γ(result)).
 * Each model is turned into a `ConcreteState` and joined into `result`
 * via `updateWith`. Widening is triggered after a configurable number
 * of iterations. The loop terminates once no counterexample model exists,
 * at which point `result` is a greatest fixpoint below the concrete
 * semantics and the original input.
 */
bool UnilateralAnalyzer::strongestConsequence(
    AbstractValue *result, const ValueMapping &vmap, z3::solver *solver,
    std::vector<z3::expr> *assumptions) const {
  bool changed = false;
  unsigned int loop_count = 0;
  auto config = FunctionContext_.getConfig();
  int widen_delay = config.get<int>("Analyzer", "WideningDelay", 20);
  int widen_frequency = config.get<int>("Analyzer", "WideningFrequency", 10);

  while (true) {
    vout << "loop iteration: " << ++loop_count << "\n";
    {
      VOutBlock vob("candidate result");
      vout << *result;
    }

    z3::expr constraint = !result->toFormula(vmap, solver->ctx());
    solver->add(constraint);

    {
      VOutBlock vob("candidate result constraint");
      vout << constraint;
    }

    auto z3_answer = checkWithStats(solver, assumptions);
    assert(z3_answer != z3::unknown);

    if (z3_answer == z3::unsat)
      break;

    vout << "model {{{\n" << solver->get_model() << "}}}\n";

    auto cstate = ConcreteState(vmap, solver->get_model());
    bool here_changed = result->updateWith(cstate);

    if (!here_changed) {
      vout << "ERROR: updateWith() returned false\n";
      {
        VOutBlock vob("faulty abstract value");
        vout << *result;
      }
      assert(here_changed);
    }

    int widen_n = loop_count - widen_delay;
    if (widen_n >= 0 && (widen_n % widen_frequency) == 0) {
      vout << "widening!\n";
      result->widen();
    }

    changed = true;
  }

  return changed;
}

} // namespace symabs_ai

/*

 * Author: rainoftime
*/
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/ConcreteState.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace symabs_ai {
namespace {
void addObjectiveIfNew(const z3::expr &objective,
                       std::vector<z3::expr> *objectives,
                       std::set<std::string> *seen) {
  std::string key = objective.to_string();
  if (seen->insert(key).second)
    objectives->push_back(objective);
}

void collectObjectives(const FunctionContext &fctx, const ValueMapping &vmap,
                       std::vector<z3::expr> *objectives,
                       unsigned max_objectives, bool enable_pair_objectives,
                       unsigned max_bit_objectives_per_var) {
  std::set<std::string> seen;
  std::vector<z3::expr> vars;
  vars.reserve(fctx.representedValues().size());

  for (llvm::Value *value : fctx.representedValues()) {
    z3::sort sort = fctx.sortForType(value->getType());
    if (!sort.is_bv())
      continue;

    z3::expr x = vmap[value];
    vars.push_back(x);

    addObjectiveIfNew(x, objectives, &seen);
    if (objectives->size() >= max_objectives)
      return;

    addObjectiveIfNew(z3::bv2int(x, false), objectives, &seen);
    if (objectives->size() >= max_objectives)
      return;

    addObjectiveIfNew(z3::bv2int(x, true), objectives, &seen);
    if (objectives->size() >= max_objectives)
      return;

    // Bit-level objectives help bitmask/congruence-like domains by forcing
    // models that witness variability/fixity of individual bits.
    unsigned bw = x.get_sort().bv_size();
    unsigned bits_to_add = std::min(max_bit_objectives_per_var, bw);
    for (unsigned bit = 0; bit < bits_to_add; ++bit) {
      addObjectiveIfNew(z3::bv2int(x.extract(bit, bit), false), objectives,
                        &seen);
      if (objectives->size() >= max_objectives)
        return;
    }
  }

  if (!enable_pair_objectives)
    return;

  for (unsigned i = 0; i < vars.size(); ++i) {
    for (unsigned j = i + 1; j < vars.size(); ++j) {
      if (vars[i].get_sort().bv_size() != vars[j].get_sort().bv_size())
        continue;

      z3::expr diff = z3::bv2int(vars[i], true) - z3::bv2int(vars[j], true);
      addObjectiveIfNew(diff, objectives, &seen);
      if (objectives->size() >= max_objectives)
        return;

      // Octagon/zone-style template.
      z3::expr sum = z3::bv2int(vars[i], true) + z3::bv2int(vars[j], true);
      addObjectiveIfNew(sum, objectives, &seen);
      if (objectives->size() >= max_objectives)
        return;
    }
  }
}

unsigned getPositiveConfigOrDefault(const configparser::Config &cfg,
                                    const char *section, const char *key,
                                    unsigned default_value) {
  int value = cfg.get<int>(section, key, (int)default_value);
  return value > 0 ? (unsigned)value : default_value;
}

} // namespace

OMTAnalyzer::OptimizeStatus OMTAnalyzer::runOptimizeWithRetry(
    const OMTAnalyzer *analyzer, const z3::expr &objective, const z3::expr &phi,
    const ValueMapping &vmap, AbstractValue *target, bool maximize,
    unsigned timeout_ms, bool retry_unknown) {
  auto res =
      analyzer->runOptimize(objective, phi, vmap, target, maximize, timeout_ms);
  if (res == OMTAnalyzer::OptimizeStatus::Unknown && retry_unknown &&
      timeout_ms > 0) {
    return analyzer->runOptimize(objective, phi, vmap, target, maximize, 0);
  }
  return res;
}

OMTAnalyzer::OptimizeStatus
OMTAnalyzer::runOptimize(const z3::expr &objective, const z3::expr &phi,
                         const ValueMapping &vmap, AbstractValue *target,
                         bool maximize, unsigned timeout_ms) const {
  z3::optimize opt(phi.ctx());
  opt.add(phi);

  z3::params params(phi.ctx());
  params.set("priority", "box");
  if (timeout_ms > 0)
    params.set("timeout", timeout_ms);
  opt.set(params);

  if (maximize)
    opt.maximize(objective);
  else
    opt.minimize(objective);

  // Keep SMT call accounting comparable with other analyzers.
  countSmtSolverCall();
  auto res = opt.check();
  if (res == z3::sat) {
    ConcreteState cstate(vmap, opt.get_model());
    target->updateWith(cstate);
    return OptimizeStatus::Sat;
  }

  if (res == z3::unsat)
    return OptimizeStatus::Unsat;

  return OptimizeStatus::Unknown;
}

bool OMTAnalyzer::overapproximateToTop(AbstractValue *result) const {
  auto top = std::unique_ptr<AbstractValue>(result->clone());
  top->havoc();
  return result->joinWith(*top);
}

bool OMTAnalyzer::strongestConsequence(AbstractValue *result, z3::expr phi,
                                       const ValueMapping &vmap) const {
  z3::context &ctx = phi.ctx();
  auto cfg = FunctionContext_.getConfig();
  unsigned timeout_ms =
      getPositiveConfigOrDefault(cfg, "Analyzer", "OMTTimeoutMs", 10000);
  unsigned max_objectives =
      getPositiveConfigOrDefault(cfg, "Analyzer", "OMTMaxObjectives", 512);
  bool pair_objectives = cfg.get<bool>("Analyzer", "OMTPairObjectives", true);
  unsigned max_bit_objectives_per_var = getPositiveConfigOrDefault(
      cfg, "Analyzer", "OMTMaxBitObjectivesPerVar", 8);
  bool retry_unknown_without_timeout =
      cfg.get<bool>("Analyzer", "OMTRetryUnknownWithoutTimeout", true);

  z3::solver feasibility(ctx);
  feasibility.add(phi);
  auto feas_res = checkWithStats(&feasibility);

  if (feas_res == z3::unsat) {
    // Match Unilateral/Bilateral behavior in Analyzer::bestTransformer:
    // joining with alpha(false) should leave result unchanged.
    return false;
  }

  if (feas_res == z3::unknown)
    return overapproximateToTop(result);

  std::vector<z3::expr> objectives;
  collectObjectives(FunctionContext_, vmap, &objectives, max_objectives,
                    pair_objectives, max_bit_objectives_per_var);
  if (objectives.empty())
    return overapproximateToTop(result);

  auto candidate = std::unique_ptr<AbstractValue>(result->clone());
  candidate->resetToBottom();

  bool saw_unknown = false;
  for (auto &obj : objectives) {
    auto max_res =
        runOptimizeWithRetry(this, obj, phi, vmap, candidate.get(), true,
                             timeout_ms, retry_unknown_without_timeout);
    auto min_res =
        runOptimizeWithRetry(this, obj, phi, vmap, candidate.get(), false,
                             timeout_ms, retry_unknown_without_timeout);

    if (max_res == OptimizeStatus::Unsat || min_res == OptimizeStatus::Unsat) {
      // phi satisfiable was checked above; objective optimization being UNSAT
      // indicates an inconsistent optimize query state. Use a conservative
      // overapproximation to preserve soundness.
      return overapproximateToTop(result);
    }

    if (max_res == OptimizeStatus::Unknown ||
        min_res == OptimizeStatus::Unknown) {
      saw_unknown = true;
    }
  }

  if (saw_unknown)
    return overapproximateToTop(result);

  return result->joinWith(*candidate);
}

} // namespace symabs_ai

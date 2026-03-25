
#include "Checker/Pulse/Checker/PulseChecker.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Interproc/PulseModels.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// PulseChecker (Infer Pulse-inspired "incorrectness logic")
//
// This driver performs a bounded disjunctive symbolic execution over LLVM IR.
// Key properties:
// - The analysis aims at *witnessable* bugs: reports should correspond to at
//   least one feasible execution (sound incorrectness), rather than "may" bugs.
// - The abstract state is biabductive (see `AbductiveDomain`): reads may
//   populate preconditions that later become requirements on callers.
// - At control-flow merges, path conditions must be joined as a disjunction
//   approximation (keep stable facts), not conjoined.
// - Path explosion is controlled via `kMaxDisjuncts`, widening, and summary
//   construction; these knobs trade recall for scalability.
//===----------------------------------------------------------------------===//

constexpr unsigned PulseChecker::kMaxDisjuncts;
constexpr unsigned PulseChecker::kMaxCallDepth;

PulseChecker::PulseChecker(llvm::Module *M, lotus::AliasAnalysisWrapper *AA)
    : module_(M), aa_(AA), ops_(&factory_),
      models_(std::make_unique<PulseModels>(*this)) {
  registerBugTypes();
  if (aa_ && aa_->isInitialized()) {
    factory_.setMustAliasFn(
        [this](const llvm::Value *v1, const llvm::Value *v2) {
          return aa_->mustAlias(v1, v2);
        });
  }
}

PulseChecker::~PulseChecker() {
  // Flush diagnostics at end of analysis. Latent issues are intentionally not
  // auto-reported here: they must only become reports when caller context makes
  // them manifest.
  DiagnosticManager::getInstance().flush();
}

void PulseChecker::registerBugTypes() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  auto &diagMgr = DiagnosticManager::getInstance();

  useAfterFreeTypeId_ =
      mgr.register_bug_type(IssueType::UseAfterFree, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-416");
  diagMgr.registerBugType(IssueType::UseAfterFree, useAfterFreeTypeId_);

  nullDerefTypeId_ =
      mgr.register_bug_type(IssueType::NullDereference, BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "CWE-476");
  diagMgr.registerBugType(IssueType::NullDereference, nullDerefTypeId_);

  uninitializedReadTypeId_ = mgr.register_bug_type(
      IssueType::UninitializedRead, BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "CWE-457");
  diagMgr.registerBugType(IssueType::UninitializedRead,
                          uninitializedReadTypeId_);

  unnecessaryCopyTypeId_ =
      mgr.register_bug_type(IssueType::UnnecessaryCopy, BugDescription::BI_LOW,
                            BugDescription::BC_PERFORMANCE, "");
  diagMgr.registerBugType(IssueType::UnnecessaryCopy, unnecessaryCopyTypeId_);

  constRefableParamTypeId_ =
      mgr.register_bug_type("Const-Refable Parameter", BugDescription::BI_LOW,
                            BugDescription::BC_PERFORMANCE, "");
  // Note: Const-refable param doesn't have a constant in PulseDiagnostic yet,
  // or we can add one. For now, skipping registration in diagMgr or adding it.

  taintErrorTypeId_ =
      mgr.register_bug_type(IssueType::TaintError, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-20");
  diagMgr.registerBugType(IssueType::TaintError, taintErrorTypeId_);

  stackAddressEscapeTypeId_ = mgr.register_bug_type(
      IssueType::StackVariableAddressEscape, BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-562");
  diagMgr.registerBugType(IssueType::StackVariableAddressEscape,
                          stackAddressEscapeTypeId_);

  invalidFreeTypeId_ =
      mgr.register_bug_type(IssueType::InvalidFree, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-590");
  diagMgr.registerBugType(IssueType::InvalidFree, invalidFreeTypeId_);

  outOfBoundsTypeId_ =
      mgr.register_bug_type(IssueType::OutOfBounds, BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "CWE-119");
  diagMgr.registerBugType(IssueType::OutOfBounds, outOfBoundsTypeId_);
}

void PulseChecker::analyze() {
  PulseLogger::info("Starting module analysis");
  PulseLogger::incrementCounter("modules.analyzed");

  std::vector<const llvm::Function *> functions;
  functions.reserve(module_->size());
  for (auto &F : *module_) {
    if (!F.isDeclaration())
      functions.push_back(&F);
  }

  std::unordered_set<const llvm::Function *> function_set(functions.begin(),
                                                          functions.end());

  // Build direct call graph: caller -> callee (only for defined functions).
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      edges;
  edges.reserve(functions.size());
  for (const llvm::Function *F : functions) {
    std::vector<const llvm::Function *> callees;
    for (const auto &BB : *F) {
      for (const auto &I : BB) {
        auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
        if (!CI)
          continue;
        const llvm::Function *Callee = CI->getCalledFunction();
        if (!Callee || Callee->isDeclaration())
          continue;
        if (function_set.count(Callee) == 0)
          continue;
        callees.push_back(Callee);
      }
    }
    edges.emplace(F, std::move(callees));
  }

  // Reverse edges for SCC computation (Kosaraju).
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      rev_edges;
  rev_edges.reserve(functions.size());
  for (const llvm::Function *F : functions) {
    (void)rev_edges[F];
  }
  for (const auto &kv : edges) {
    const llvm::Function *caller = kv.first;
    for (const llvm::Function *callee : kv.second) {
      rev_edges[callee].push_back(caller);
    }
  }

  // First DFS pass: finish order.
  std::vector<const llvm::Function *> order;
  order.reserve(functions.size());
  std::unordered_set<const llvm::Function *> visited;
  visited.reserve(functions.size());

  std::function<void(const llvm::Function *)> dfs1 =
      [&](const llvm::Function *F) {
        if (visited.count(F))
          return;
        visited.insert(F);
        auto it = edges.find(F);
        if (it != edges.end()) {
          for (const llvm::Function *callee : it->second) {
            dfs1(callee);
          }
        }
        order.push_back(F);
      };

  for (const llvm::Function *F : functions)
    dfs1(F);

  // Second DFS pass on reverse graph: collect SCCs.
  std::unordered_set<const llvm::Function *> visited2;
  visited2.reserve(functions.size());
  std::vector<std::vector<const llvm::Function *>> sccs;
  sccs.reserve(functions.size());

  std::function<void(const llvm::Function *,
                     std::vector<const llvm::Function *> &)>
      dfs2 = [&](const llvm::Function *F,
                 std::vector<const llvm::Function *> &scc) {
        if (visited2.count(F))
          return;
        visited2.insert(F);
        scc.push_back(F);
        auto it = rev_edges.find(F);
        if (it != rev_edges.end()) {
          for (const llvm::Function *pred : it->second) {
            dfs2(pred, scc);
          }
        }
      };

  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const llvm::Function *F = *it;
    if (visited2.count(F))
      continue;
    std::vector<const llvm::Function *> scc;
    dfs2(F, scc);
    sccs.push_back(std::move(scc));
  }

  // Map function -> SCC id.
  std::unordered_map<const llvm::Function *, size_t> scc_id;
  scc_id.reserve(functions.size());
  for (size_t i = 0; i < sccs.size(); ++i) {
    for (const llvm::Function *F : sccs[i])
      scc_id[F] = i;
  }

  // Build SCC DAG (caller SCC -> callee SCC), then topo-sort it.
  std::vector<std::unordered_set<size_t>> scc_edges(sccs.size());
  std::vector<size_t> indeg(sccs.size(), 0);
  for (const auto &kv : edges) {
    const llvm::Function *caller = kv.first;
    size_t c_id = scc_id[caller];
    for (const llvm::Function *callee : kv.second) {
      size_t d_id = scc_id[callee];
      if (c_id == d_id)
        continue;
      if (scc_edges[c_id].insert(d_id).second) {
        indeg[d_id]++;
      }
    }
  }

  std::queue<size_t> q;
  for (size_t i = 0; i < indeg.size(); ++i) {
    if (indeg[i] == 0)
      q.push(i);
  }
  std::vector<size_t> topo;
  topo.reserve(sccs.size());
  while (!q.empty()) {
    size_t id = q.front();
    q.pop();
    topo.push_back(id);
    for (size_t succ : scc_edges[id]) {
      if (--indeg[succ] == 0)
        q.push(succ);
    }
  }

  // Analyze SCCs in reverse-topological order (callees first).
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    size_t id = *it;
    current_scc_.clear();
    for (const llvm::Function *F : sccs[id])
      current_scc_.insert(F);
    for (const llvm::Function *F : sccs[id]) {
      analyzeFunction(F);
    }
  }
  current_scc_.clear();

  PulseLogger::info("Completed analysis of " +
                    std::to_string(functions.size()) + " functions");

  // Flush periodically or at end
  DiagnosticManager::getInstance().flush();
}

void PulseChecker::analyzeFunction(const llvm::Function *F) {
  PulseLogger::logFunction(F, "starting analysis");
  PulseLogger::startTimer("function." + F->getName().str());
  PulseLogger::incrementCounter("functions.analyzed");

  // Skip if already has summary (avoid re-analysis)
  if (summary_manager_.hasSummary(F)) {
    PulseLogger::logFunction(F, "skipped (already has summary)");
    return;
  }

  analysis_non_disj_.clear();

  LoopAbstraction loop_abs;
  {
    llvm::Function &Fn = *const_cast<llvm::Function *>(F);
    llvm::DominatorTree DT(Fn);
    llvm::LoopInfo LI;
    LI.analyze(DT);
    loop_abs.initialize(LI);
  }
  loop_abstractions_[F] = std::move(loop_abs);
  LoopAbstraction &loop_abs_ref = loop_abstractions_[F];

  // Initialize disjunctive domain
  DisjunctiveDomain &disj_domain = disjunctive_domains_[F];
  disj_domain.clear();

  using WorkItem = std::tuple<const llvm::BasicBlock *, ExecutionDomain,
                              const llvm::BasicBlock *>;
  std::queue<WorkItem> worklist;
  const llvm::BasicBlock *entry_block = &F->getEntryBlock();
  ExecutionDomain init_state = initializeFunction(F);
  worklist.push(std::make_tuple(entry_block, std::move(init_state),
                                (const llvm::BasicBlock *)nullptr));

  std::vector<ExecutionDomain> exit_states;
  std::vector<ExecutionDomain> latent_exit_states;
  std::map<const llvm::BasicBlock *, unsigned> block_visits;
  unsigned iter_limit = 0;
  const unsigned max_iter = 100000;

  PulseLogger::incrementCounter("paths.explored");

  while (!worklist.empty() && iter_limit++ < max_iter) {
    if (worklist.size() > kMaxDisjuncts * 4) {
      PulseLogger::warning("Pruning oversized worklist for function " +
                           F->getName().str());
      while (worklist.size() > kMaxDisjuncts * 2) {
        worklist.pop();
      }
    }

    WorkItem item = std::move(worklist.front());
    worklist.pop();
    const llvm::BasicBlock *BB = std::get<0>(item);
    ExecutionDomain current_state = std::move(std::get<1>(item));
    const llvm::BasicBlock *pred_bb = std::get<2>(item);

    if (current_state.isStopped())
      continue;

    // Record entry predecessor for sound PHI/select handling.
    current_state.setEntryPred(pred_bb);

    block_visits[BB]++;
    if (block_visits[BB] > kMaxDisjuncts * 4)
      continue;

    if (loop_abs_ref.isLoopHeader(BB)) {
      bool should_widen = loop_abs_ref.visitHeader(BB, current_state);

      if (should_widen) {
        // Check if we should infer invariant
        if (loop_abs_ref.isInferringInvariant(BB)) {
          auto invariant_opt = loop_abs_ref.inferInvariant(
              BB, loop_abs_ref.getEntryState(BB), current_state);
          if (invariant_opt) {
            // Only adopt an inferred invariant if it preserves the current
            // incoming edge context. Otherwise, PHI nodes could be evaluated
            // with a predecessor different from the witness that reached BB.
            if (invariant_opt->getEntryPred() == pred_bb) {
              current_state = std::move(*invariant_opt);
            }
          }
        } else {
          // Apply widening
          ExecutionDomain widened = loop_abs_ref.widen(BB, current_state);
          if (widened.getEntryPred() == pred_bb) {
            current_state = std::move(widened);
          }
        }
        if (current_state.isStopped()) {
          continue;
        }
      }
    }

    // Track iteration for widening
    disj_domain.widen(BB);

    // Check if we should widen at this block
    if (disj_domain.shouldWiden(BB)) {
      // After widening, join all disjuncts at this block
      ExecutionDomain joined = disj_domain.joinAtBlock(BB);
      if (!joined.isStopped()) {
        // Preserve the original predecessor context for PHI handling.
        // The joined state may have a stale entry pred from a previous
        // iteration. We must keep the current edge's pred_bb to ensure sound
        // PHI evaluation.
        joined.setEntryPred(pred_bb);
        current_state = std::move(joined);
      } else {
        continue;
      }
    } else {
      // Add current state to disjunctive domain for this block
      disj_domain.add(BB, current_state.clone(), pred_bb);
    }

    std::vector<ExecutionDomain> states;
    states.push_back(std::move(current_state));

    auto limitStates = [&](std::vector<ExecutionDomain> &vec) {
      if (vec.size() <= kMaxDisjuncts) {
        return;
      }
      std::vector<ExecutionDomain> preferred;
      std::vector<ExecutionDomain> rest;
      preferred.reserve(vec.size());
      rest.reserve(vec.size());
      for (auto &st : vec) {
        auto *a = st.getAstate();
        if (a && !a->hasUnknownValues()) {
          preferred.push_back(std::move(st));
        } else {
          rest.push_back(std::move(st));
        }
      }
      vec.clear();
      for (auto &st : preferred) {
        if (vec.size() >= kMaxDisjuncts)
          break;
        vec.push_back(std::move(st));
      }
      for (auto &st : rest) {
        if (vec.size() >= kMaxDisjuncts)
          break;
        vec.push_back(std::move(st));
      }
    };

    for (const llvm::Instruction &I : *BB) {
      if (states.empty())
        break;

      std::vector<ExecutionDomain> next_states;
      next_states.reserve(states.size());

      for (auto &st : states) {
        if (st.isStopped()) {
          next_states.push_back(std::move(st));
          continue;
        }

        const llvm::BasicBlock *phi_pred = pred_bb;
        if (llvm::isa<llvm::PHINode>(&I) && !phi_pred) {
          auto it = pred_begin(BB);
          if (it != pred_end(BB))
            phi_pred = *it;
        }

        auto new_states = executeInstruction(&I, std::move(st), phi_pred, 0u);
        if (!new_states.empty()) {
          for (auto &ns : new_states) {
            next_states.push_back(std::move(ns));
          }
        }
      }

      states = std::move(next_states);
      limitStates(states);
    }

    bool any_continuing = false;
    for (auto &st : states) {
      if (st.isStopped()) {
        if (st.isExitProgram()) {
          exit_states.push_back(st.clone());
        } else if (st.isLatentAbortProgram() || st.isLatentInvalidAccess()) {
          latent_exit_states.push_back(st.clone());
        }
        continue;
      }
      any_continuing = true;
    }

    if (!any_continuing) {
      continue;
    }

    const llvm::Instruction *term = BB->getTerminator();
    if (!term)
      continue;

    auto *BI =
        llvm::dyn_cast<llvm::BranchInst>(const_cast<llvm::Instruction *>(term));
    if (BI && BI->isConditional() && BI->getNumSuccessors() == 2) {
      for (auto &st : states) {
        if (st.isStopped())
          continue;
        for (unsigned i = 0; i < 2; i++) {
          llvm::Optional<ExecutionDomain> fork_opt =
              applyBranchCondition(st.clone(), BI, i, pred_bb);
          if (!fork_opt)
            continue;
          const llvm::BasicBlock *succ = BI->getSuccessor(i);
          if (succ->empty())
            continue;
          fork_opt->setEntryPred(BB);
          worklist.push(std::make_tuple(succ, std::move(*fork_opt), BB));
        }
      }
    } else {
      for (auto &st : states) {
        if (st.isStopped())
          continue;
        for (const llvm::BasicBlock *succ : llvm::successors(BB)) {
          if (succ->empty())
            continue;
          ExecutionDomain succ_state = st.clone();
          succ_state.setEntryPred(BB);
          worklist.push(std::make_tuple(succ, std::move(succ_state), BB));
        }
      }
    }
  }

  if (!exit_states.empty() || !latent_exit_states.empty())
    createSummary(F, exit_states, latent_exit_states);

  reportUnnecessaryCopies(F);
  reportConstRefableParams(F);

  PulseLogger::endTimer("function." + F->getName().str());
  PulseLogger::logFunction(F, "completed analysis");
}

ExecutionDomain PulseChecker::initializeFunction(const llvm::Function *F) {
  ExecutionDomain exec_state;
  auto *astate = exec_state.getAstate();
  for (auto &Arg : F->args()) {
    if (Arg.getType()->isPointerTy()) {
      AbstractValue av = factory_.getOrCreate(&Arg);
      Address addr(av);
      astate->getPostStack().add(&Arg, addr);
      astate->getPostAttrs().add(av, Attribute::Uninitialized);
    }
  }
  return exec_state;
}

std::vector<std::pair<ExecutionDomain, llvm::Optional<AbstractValue>>>
PulseChecker::runCallee(const llvm::Function *callee,
                        const ExecutionDomain &caller_state,
                        const llvm::CallInst *CI, const llvm::BasicBlock *pred,
                        unsigned call_depth) {
  std::vector<std::pair<ExecutionDomain, llvm::Optional<AbstractValue>>> result;
  if (call_depth >= kMaxCallDepth)
    return result;

  // Detect recursive calls: if callee is in the current SCC being analyzed,
  // treat it as unknown to prevent infinite recursion.
  if (current_scc_.count(callee) > 0) {
    ExecutionDomain state = caller_state.clone();
    auto *astate = state.getAstate();
    if (astate) {
      astate->addRecursiveCall(callee->getName().str());
      astate->declareUnknownValues();
    }
    // Create a fresh return value for the unknown recursive call
    if (CI->getType()->isPointerTy()) {
      AbstractValue ret_val = factory_.createFresh(CI);
      Address ret_addr(ret_val);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      if (auto *astate = state.getAstate()) {
        astate->getPostStack().add(CI, ret_addr);
      }
    }
    result.push_back({std::move(state), llvm::Optional<AbstractValue>()});
    return result;
  }

  ExecutionDomain init = initializeFunction(callee);
  auto *init_astate = init.getAstate();
  ExecutionDomain caller_eval_state = caller_state.clone();
  auto *caller_eval_astate = caller_eval_state.getAstate();
  if (!init_astate || !caller_eval_astate)
    return result;

  const auto *ai = callee->arg_begin();
  const auto *ae = callee->arg_end();
  unsigned i = 0;
  for (; ai != ae && i < CI->arg_size(); ++ai, ++i) {
    if (!ai->getType()->isPointerTy())
      continue;
    // Evaluate actuals in the caller's state (not in the fresh callee state).
    auto opt = ops_.eval(*caller_eval_astate, CI->getArgOperand(i), CI, pred);
    if (opt) {
      init_astate->getPostStack().add(&*ai, *opt);
      AbstractValue formal_av = factory_.getOrCreate(&*ai);
      AbstractValue formal_canon = init_astate->getCanonical(formal_av);
      init_astate->getPostAttrs().remove(formal_av, Attribute::Uninitialized);

      // Track null arguments: if the actual argument is a null constant or has
      // Null attribute, mark the formal parameter as potentially null
      AbstractValue actual_canon = init_astate->getCanonical(opt->addr);
      if (init_astate->getPostAttrs().has(actual_canon, Attribute::Null) ||
          init_astate->getPathFormula().isNull(actual_canon)) {
        // Check if this is a null constant source
        if (PulseOperations::isNullConstantSource(*opt)) {
          init_astate->getPostAttrs().add(formal_canon, Attribute::Null);
          init_astate->getPathFormula().addNull(formal_canon);
        }
      }
    }
  }

  // Use block-based worklist for proper CFG traversal
  std::map<const llvm::BasicBlock *, std::vector<ExecutionDomain>>
      block_entry_states;
  block_entry_states[&callee->getEntryBlock()].push_back(std::move(init));

  std::queue<const llvm::BasicBlock *> worklist;
  worklist.push(&callee->getEntryBlock());
  std::set<const llvm::BasicBlock *> processed;

  unsigned iter_limit = 0;
  const unsigned max_iter = 50000;

  while (!worklist.empty() && iter_limit++ < max_iter) {
    const llvm::BasicBlock *BB = worklist.front();
    worklist.pop();

    if (processed.count(BB) && block_entry_states[BB].empty())
      continue;

    std::vector<ExecutionDomain> entry_states =
        std::move(block_entry_states[BB]);
    block_entry_states[BB].clear();

    if (entry_states.empty())
      continue;

    // Sound incorrectness: process multiple entry states to preserve witnesses.
    // Instead of dropping all but the first, we limit to kMaxDisjuncts and
    // process each as a separate execution path. This avoids losing bug
    // witnesses that would be reachable through other entry paths.
    auto selectRepresentative = [](std::vector<ExecutionDomain> &states) {
      if (states.size() <= 1)
        return;
      std::vector<ExecutionDomain> preferred;
      std::vector<ExecutionDomain> rest;
      preferred.reserve(states.size());
      rest.reserve(states.size());
      for (auto &st : states) {
        auto *a = st.getAstate();
        if (a && !a->hasUnknownValues()) {
          preferred.push_back(std::move(st));
        } else {
          rest.push_back(std::move(st));
        }
      }
      states.clear();
      for (auto &st : preferred) {
        if (states.size() >= kMaxDisjuncts)
          break;
        states.push_back(std::move(st));
      }
      for (auto &st : rest) {
        if (states.size() >= kMaxDisjuncts)
          break;
        states.push_back(std::move(st));
      }
    };
    selectRepresentative(entry_states);

    // Filter out stopped states early
    std::vector<ExecutionDomain> work_states;
    work_states.reserve(entry_states.size());
    for (auto &st : entry_states) {
      if (!st.isStopped()) {
        work_states.push_back(std::move(st));
      }
    }

    if (work_states.empty())
      continue;

    // Process instructions in block with multiple states
    std::vector<ExecutionDomain> states = std::move(work_states);

    auto limitStates = [&](std::vector<ExecutionDomain> &vec) {
      if (vec.size() <= kMaxDisjuncts)
        return;
      std::vector<ExecutionDomain> preferred;
      std::vector<ExecutionDomain> rest;
      preferred.reserve(vec.size());
      rest.reserve(vec.size());
      for (auto &st : vec) {
        auto *a = st.getAstate();
        if (a && !a->hasUnknownValues()) {
          preferred.push_back(std::move(st));
        } else {
          rest.push_back(std::move(st));
        }
      }
      vec.clear();
      for (auto &st : preferred) {
        if (vec.size() >= kMaxDisjuncts)
          break;
        vec.push_back(std::move(st));
      }
      for (auto &st : rest) {
        if (vec.size() >= kMaxDisjuncts)
          break;
        vec.push_back(std::move(st));
      }
    };

    for (const llvm::Instruction &I : *BB) {
      if (states.empty())
        break;

      std::vector<ExecutionDomain> next_states;
      next_states.reserve(states.size());

      for (auto &st : states) {
        if (st.isStopped()) {
          next_states.push_back(std::move(st));
          continue;
        }

        const llvm::BasicBlock *pred_bb = nullptr;
        if (llvm::isa<llvm::PHINode>(&I)) {
          if (pred_begin(BB) != pred_end(BB)) {
            pred_bb = *pred_begin(BB);
          }
        }

        auto new_states =
            executeInstruction(&I, std::move(st), pred_bb, call_depth + 1);
        if (!new_states.empty()) {
          for (auto &ns : new_states) {
            next_states.push_back(std::move(ns));
          }
        }
      }

      states = std::move(next_states);
      limitStates(states);
    }

    // Collect returns / propagate to successors
    bool any_continuing = false;
    for (auto &st : states) {
      if (st.isStopped()) {
        if (st.isExitProgram()) {
          result.push_back({st.clone(),
                            st.getStoppedExecution().return_value});
        }
        continue;
      }
      any_continuing = true;
    }

    if (any_continuing && BB->getTerminator()) {
      for (auto &st : states) {
        if (st.isStopped())
          continue;
        for (const llvm::BasicBlock *succ : llvm::successors(BB)) {
          if (succ->empty())
            continue;
          block_entry_states[succ].push_back(st.clone());
          if (processed.find(succ) == processed.end() ||
              !block_entry_states[succ].empty()) {
            worklist.push(succ);
          }
        }
      }
    }

    processed.insert(BB);

    if (worklist.size() > kMaxDisjuncts) {
      while (worklist.size() > kMaxDisjuncts) {
        worklist.pop();
      }
    }
  }

  return result;
}

void PulseChecker::reportBug(OperationResult kind, const llvm::Instruction *loc,
                             AbstractValue addr, const Trace &trace,
                             const AbductiveDomain *astate) {
  PulseLogger::logBug(kind, loc);
  PulseLogger::incrementCounter("bugs.total");

  std::unique_ptr<Diagnostic> diagnostic;

  switch (kind) {
  case OperationResult::UseAfterFree: {
    InvalidationKind invKind = InvalidationKind::Other;
    if (astate) {
      AbstractValue canon = astate->getCanonical(addr);
      auto inv = astate->getInvalidationInfo(canon);
      if (inv)
        invKind = inv->first;
    }
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Use after free detected", "Access to freed memory",
        "Ensure the memory is not freed before access", IssueType::UseAfterFree,
        trace.clone(), invKind);
    break;
  }
  case OperationResult::NullDereference: {
    // For NPD checker, only show null constant sources in the trace
    Trace filtered_trace;
    for (const auto &event : trace.getEvents()) {
      if (!event.location)
        continue;
      // Only include Store events where a null constant is stored
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(event.location)) {
        const llvm::Value *stored_value = SI->getValueOperand();
        // Check if storing a null pointer constant (not integer 0).
        if (detail::isNullPointerConstantValue(stored_value)) {
          filtered_trace.addEvent(event.location, event.function,
                                  "Null constant stored");
        }
      }
    }
    // Always add the dereference location as the sink
    filtered_trace.addEvent(loc, loc ? loc->getFunction() : nullptr,
                            "Null pointer dereference");
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Null pointer dereference", "Pointer is null",
        "Check for null before dereferencing", IssueType::NullDereference,
        std::move(filtered_trace));
    break;
  }
  case OperationResult::UninitializedRead:
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Uninitialized read", "Reading uninitialized memory",
        "Initialize variable before use", IssueType::UninitializedRead,
        trace.clone());
    break;
  case OperationResult::OutOfBounds:
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Out of bounds access", "Access beyond allocated bounds",
        "Ensure indices and lengths stay within the allocated object",
        IssueType::OutOfBounds, trace.clone());
    break;
  case OperationResult::TaintError:
    diagnostic = std::make_unique<TaintFlow>(loc, "Unknown Source",
                                             "Unknown Sink", trace.clone());
    break;
  default:
    return;
  }

  if (diagnostic) {
    DiagnosticManager::getInstance().report(std::move(diagnostic));
  }
}

void PulseChecker::reportUnnecessaryCopies(const llvm::Function *F) {
  (void)F;
  const auto &stores = analysis_non_disj_.getCopiedStores();
  for (const llvm::StoreInst *SI : stores) {
    // Skip reporting unnecessary copies for pointer types (reduce false
    // positives)
    if (SI->getValueOperand()->getType()->isPointerTy())
      continue;

    // Skip if storing a constant (not really a copy)
    if (llvm::isa<llvm::Constant>(SI->getValueOperand()))
      continue;

    // Skip if storing to/from alloca (local variable initialization)
    if (llvm::isa<llvm::AllocaInst>(SI->getPointerOperand()))
      continue;

    // Skip if this is a PHI node result (common in SSA form)
    if (llvm::isa<llvm::PHINode>(SI->getValueOperand()))
      continue;

    // Create Diagnostic for unnecessary copy
    // For now, assume variable name extraction from IR
    std::string varName = "variable";
    std::string typeName = "type";
    auto diag = std::make_unique<UnnecessaryCopy>(
        static_cast<const llvm::Instruction *>(SI), varName, typeName);
    DiagnosticManager::getInstance().report(std::move(diag));
  }
}

void PulseChecker::reportConstRefableParams(const llvm::Function *F) {
  (void)F;
  for (const llvm::Argument *A : analysis_non_disj_.getConstRefableParams()) {
    const llvm::Instruction *firstUse =
        A->getParent()->getEntryBlock().getFirstNonPHI();
    if (!firstUse)
      continue;

    // This is a special case not fully covered by PulseDiagnostic yet in my
    // port, but we can add it or reuse UnnecessaryCopy Or just log it for now
    // TODO: Add ConstRefableParam to PulseDiagnostic
  }
}

void PulseChecker::reportDiagnostic(const llvm::Instruction *loc,
                                    const std::string &message,
                                    const std::string &type, int confidence) {
  (void)confidence;
  // For TaintSink, specialized handling
  if (type == "TaintSink") {
    Trace trace;
    trace.addEvent(loc, message);
    auto diag = std::make_unique<TaintFlow>(loc, "Taint Source", "Taint Sink",
                                            std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
  } else {
    // Generic diagnostic reporting via stderr or log
    // Or create a GenericDiagnostic class
    llvm::errs() << "[Pulse] " << type << ": " << message << " at " << *loc
                 << "\n";
  }
}

void PulseChecker::reportDiagnostic(const Diagnostic &diagnostic) {
  (void)diagnostic;
  // Backward-compatibility overload: structured diagnostics should be
  // constructed and reported via DiagnosticManager.
}

} // namespace pulse

/// @file InterProceduralPass.cpp
/// @brief LLVM module pass implementing inter-procedural pointer analysis with
/// call graph construction
///
/// This file implements `LotusAA`, the top-level LLVM ModulePass that
/// orchestrates
/// **whole-program pointer analysis** and **on-the-fly call graph
/// construction**.
///
/// **Pass Architecture:**
/// ```
/// LotusAA::runOnModule(Module)
///   ├── Initialize global structures (NullObj, UnknownObj, sentinel values)
///   ├── computeGlobalHeuristic() - Cache constant stores into globals
///   ├── computePtsCgIteratively() - Main fixpoint algorithm
///   │   ├── initFuncProcessingSeq() - Build call graph, topological sort
///   │   ├── For each function (bottom-up):
///   │   │   └── computePTA(func) - Run intra-procedural analysis
///   │   ├── computeCG() - Resolve indirect calls
///   │   ├── Detect changes, iterate until fixpoint
///   │   └── detectBackEdges() - Handle recursion
///   └── finalizeCg() - Print results (if enabled)
/// ```
///
/// **On-the-Fly Call Graph Construction:**
/// The analysis alternates between pointer analysis and call graph refinement:
/// 1. Analyze functions bottom-up using current call graph
/// 2. Resolve indirect calls using pointer analysis results
/// 3. Update call graph with newly discovered edges
/// 4. Reanalyze affected functions
/// 5. Repeat until fixpoint (no new edges discovered)
///
/// **Fixpoint Iteration:**
/// ```
/// iter = 0
/// changed = all_functions
/// while changed and iter < max_iter:
///   for func in bottom_up_order:
///     if func in changed:
///       interface_changed = analyze(func)
///       if interface_changed:
///         changed += callers_of(func)
///   update_call_graph_from_FP_results()
///   detect_back_edges()
///   iter++
/// ```
///
/// **Key Features:**
/// - **Context-Sensitive**: Function summaries provide calling context
/// - **Flow-Sensitive**: SSA-based intra-procedural analysis
/// - **On-the-Fly CG**: No pre-computed call graph needed
/// - **Scalable**: Iterates only on changed functions
///
/// **Command-line Options:**
/// - `--lotus-cg`: Enable call graph construction (default: on)
/// - `--lotus-restrict-cg-iter`: Max CG iterations (default: 5)
/// - `--lotus-print-pts`: Print points-to results
/// - `--lotus-print-cg`: Print resolved call graph
/// - `--lotus-enable-global-heuristic`: Cache constant stores into globals
///
/// **Registered Pass:**
/// - Pass ID: "lotus-aa"
/// - Description: "LotusAA: Flow- and context-sensitive alias analysis"
///
/// @see IntraLotusAA for per-function analysis
/// @see CallGraphState for call graph management
/// @see FunctionPointerResults for storing resolved indirect calls

#include "Alias/LotusAA/Engine/InterProceduralPass.h"

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/LotusAA/MemoryModel/MemObject.h"
#include "Alias/LotusAA/MemoryModel/PointsToGraph.h"
#include "Alias/LotusAA/Support/LotusConfig.h"

#include <algorithm>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

namespace {

static bool hasSameTargetMembership(const CallTargetSet *oldTargets,
                                    const CallTargetSet &newTargets) {
  if (!oldTargets)
    return newTargets.empty();

  if (oldTargets->size() != newTargets.size())
    return false;

  for (const auto &newTarget : newTargets) {
    if (oldTargets->count(newTarget.first) == 0)
      return false;
  }

  return true;
}

} // namespace

// Command-line options
static cl::opt<bool>
    lotus_cg("lotus-cg", cl::desc("Use LotusAA to build call graph"),
             cl::init(LotusConfig::DebugOptions::DEFAULT_ENABLE_CG));

static cl::opt<int> lotus_restrict_cg_iter(
    "lotus-restrict-cg-iter",
    cl::desc("Maximum iterations for call graph construction"),
    cl::init(LotusConfig::CallGraphLimits::DEFAULT_MAX_ITERATIONS));

static cl::opt<bool> lotus_enable_global_heuristic(
    "lotus-enable-global-heuristic",
    cl::desc("Enable heuristic for global pointer handling"),
    cl::init(LotusConfig::Heuristics::DEFAULT_ENABLE_GLOBAL_HEURISTIC));

static cl::opt<bool>
    lotus_print_pts("lotus-print-pts",
                    cl::desc("Print LotusAA points-to results"),
                    cl::init(LotusConfig::DebugOptions::DEFAULT_PRINT_PTS));

static cl::opt<bool>
    lotus_print_cg("lotus-print-cg",
                   cl::desc("Print LotusAA call graph results"),
                   cl::init(LotusConfig::DebugOptions::DEFAULT_PRINT_CG));

static cl::opt<unsigned> lotus_parallel_threads(
    "lotus-aa-threads", cl::desc("Number of threads for LotusAA (0 = auto)"),
    cl::init(1)); // Default to single-threaded to avoid concurrency bugs

char LotusAA::ID = 0;
static RegisterPass<LotusAA> X("lotus-aa",
                               "LotusAA: Flow-sensitive alias analysis",
                               false, /* CFG only */
                               true /* is analysis */);

LotusAA::LotusAA() : ModulePass(ID), DL(nullptr) {}

LotusAA::~LotusAA() {
  delete MemObject::NullObj;
  delete MemObject::UnknownObj;

  // Note: Don't delete Arguments - they're LLVM-managed
  // The sentinel values (FREE_VARIABLE, etc.) are Arguments but
  // we created them, so we shouldn't delete them either

  for (auto &func_result : intraResults_) {
    if (func_result.second)
      delete func_result.second;
  }

  // Clean up cached dominator trees
  for (auto &dt_pair : dominatorTrees_) {
    if (dt_pair.second)
      delete dt_pair.second;
  }
}

void LotusAA::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  // Dominator trees are computed and cached on-demand in getDomTree().
  // We do NOT declare DominatorTreeWrapperPass as required because we manage
  // our own DominatorTree objects (one per function) rather than using the
  // pass-manager-provided ones, which would only cover the current function.
}


bool LotusAA::runOnModule(Module &M) {
  DL = &M.getDataLayout();

  IntraLotusAAConfig::setParam();

  // Initialize spec manager
  specManager_.initialize(M);

  // Initialize global singletons
  MemObject::NullObj = new MemObject(nullptr, nullptr, MemObject::CONCRETE);
  MemObject::NullObj->findLocator(0, true);

  MemObject::UnknownObj = new MemObject(nullptr, nullptr, MemObject::CONCRETE);
  MemObject::UnknownObj->findLocator(0, true);

  LocValue::FREE_VARIABLE = new Argument(Type::getVoidTy(M.getContext()));
  LocValue::NO_VALUE = new Argument(Type::getVoidTy(M.getContext()));
  LocValue::UNDEF_VALUE = new Argument(Type::getVoidTy(M.getContext()));
  LocValue::SUMMARY_VALUE = new Argument(Type::getVoidTy(M.getContext()));

  PTGraph::DEFAULT_NON_POINTER_TYPE = Type::getInt64Ty(M.getContext());
  PTGraph::DEFAULT_POINTER_TYPE = Type::getInt8PtrTy(M.getContext());

  // Initialize results map
  for (Function &F : M) {
    intraResults_[&F] = nullptr;
  }

  // Compute global heuristics
  if (lotus_enable_global_heuristic) {
    computeGlobalHeuristic(M);
  }

  // Compute PTS and CG iteratively
  std::vector<Function *> func_seq;
  computePtsCgIteratively(M, func_seq);

  // Finalize
  finalizeCg(func_seq);

  return false;
}

void LotusAA::computeGlobalHeuristic(Module &M) {
  // Cache constant values that are explicitly
  // stored into global pointers while scanning the module. This is a passive
  // cache and does not synthesize new points-to or call-graph facts by itself.
  globalValuesCache_.clear();

  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *store = dyn_cast<StoreInst>(&I);
        if (!store)
          continue;

        Value *store_ptr = store->getPointerOperand();
        Value *store_value = store->getValueOperand();
        if (isa<GlobalValue>(store_ptr) && isa<Constant>(store_value)) {
          globalValuesCache_[store_ptr].insert(store_value);
        }
      }
    }
  }
}


void LotusAA::initFuncProcessingSeq(Module &M,
                                    std::vector<Function *> &func_seq) {
  // Build a bottom-up (callee-before-caller) ordering of functions.
  //
  // The previous implementation had two problems:
  //
  // 1. Back-edges were only detected *after* the sort (in detectBackEdges),
  //    but the sort itself needed to know which edges are back-edges to avoid
  //    counting them in in-degrees.  We now run a DFS-based back-edge
  //    detection pass first, then build the DAG for Kahn's algorithm.
  //
  // 2. Kahn's algorithm silently drops functions that are part of SCCs
  //    (mutual recursion) because their in-degree never reaches zero.  We
  //    detect these and append them at the end so they are still analysed.

  // --- Step 1: rebuild the raw call graph (all edges, no back-edge filter) ---
  callGraphState_.clear();

  std::vector<Function *> allFunctions;
  for (Function &F : M) {
    if (!F.isDeclaration())
      allFunctions.push_back(&F);
  }
  callGraphState_.initializeForFunctions(allFunctions);

  const auto &fpResults = functionPointerResults_.getResultsMap();
  for (const auto &callerResults : fpResults) {
    Function *caller = callerResults.first;
    for (const auto &callsiteResults : callerResults.second) {
      for (const auto &callee_item : callsiteResults.second) {
        Function *callee = callee_item.first;
        if (callee && !callee->isDeclaration())
          callGraphState_.addEdge(caller, callee);
      }
    }
  }

  // --- Step 2: detect back-edges via DFS so the DAG is acyclic ---
  {
    std::set<Function *> dummy;
    callGraphState_.detectBackEdges(dummy);
  }

  // --- Step 3: Kahn's topological sort on the DAG (back-edges excluded) ---
  map<Function *, int> in_degree;
  for (Function *F : allFunctions)
    in_degree[F] = 0;

  for (Function *F : allFunctions) {
    for (Function *callee : callGraphState_.getCallees(F)) {
      if (!callGraphState_.isBackEdge(F, callee))
        in_degree[callee]++;
    }
  }

  std::vector<Function *> worklist;
  for (Function *F : allFunctions) {
    if (in_degree[F] == 0)
      worklist.push_back(F);
  }

  func_seq.clear();
  std::set<Function *> inSeq;

  while (!worklist.empty()) {
    Function *F = worklist.back();
    worklist.pop_back();
    func_seq.push_back(F);
    inSeq.insert(F);

    for (Function *callee : callGraphState_.getCallees(F)) {
      if (callGraphState_.isBackEdge(F, callee))
        continue;
      if (--in_degree[callee] == 0)
        worklist.push_back(callee);
    }
  }

  // --- Step 4: append any functions dropped by Kahn's (SCC members) ---
  // These are functions whose in-degree never reached zero because they are
  // part of a cycle.  Append them in an arbitrary order so they are still
  // analysed (they will be re-analysed in subsequent fixpoint iterations).
  for (Function *F : allFunctions) {
    if (!inSeq.count(F))
      func_seq.push_back(F);
  }
}


void LotusAA::initCGBackedge() {
  // Initialize from existing call graph (direct calls)
  // Scan all functions to find direct call sites
  for (auto &func_result : intraResults_) {
    Function *F = func_result.first;
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        if (CallBase *call = dyn_cast<CallBase>(&I)) {
          if (Function *callee = call->getCalledFunction()) {
            functionPointerResults_.addTarget(F, call, callee);
          }
        }
      }
    }
  }
}

void LotusAA::computePtsCgIteratively(Module &M,
                                      std::vector<Function *> &func_seq) {
  initCGBackedge();

  bool changed = true;
  int iteration = 0;
  set<Function *> changed_func;

  // Initialize: analyze all functions
  for (Function &F : M) {
    changed_func.insert(&F);
  }

  // Always use sequential analysis to avoid concurrency bugs
  const unsigned poolMax = 1;

  while (changed) {
    outs() << "[LotusAA] Iteration " << (iteration + 1) << " using " << poolMax
           << " thread(s)\n";

    initFuncProcessingSeq(M, func_seq);
    changed = false;

    // Sequential analysis: process functions in bottom-up order.
    // func_seq is caller-first, so walk it in reverse to analyze callees first.
    for (int i = (int)func_seq.size() - 1; i >= 0; i--) {
      Function *func = func_seq[i];
      bool needsAnalysis =
          (iteration >= lotus_restrict_cg_iter) || changed_func.count(func);

      if (!needsAnalysis)
        continue;

      IntraLotusAA *old_result = intraResults_[func];
      IntraLotusAA *new_result = new IntraLotusAA(func, this);
      new_result->computePTA();
      if (lotus_cg)
        new_result->computeCG();

      // Update results
      intraResults_[func] = new_result;
      if (old_result && old_result != new_result)
        delete old_result;

      // Use conservative invalidation: any analyzed callee can
      // require its callers to be revisited later in the same bottom-up pass.
      for (Function *caller : callGraphState_.getCallers(func)) {
        if (!callGraphState_.isBackEdge(caller, func)) {
          changed_func.insert(caller);
        }
      }
    }

    outs() << "\n";

    // Caller reprocessing markers are only relevant within the current
    // bottom-up pass. Cross-iteration progress is driven by call-target
    // membership changes.
    changed_func.clear();

    // Update CG if enabled
    if (iteration >= lotus_restrict_cg_iter)
      break;

    if (lotus_cg) {
      for (int i = (int)func_seq.size() - 1; i >= 0; i--) {
        Function *func = func_seq[i];

        IntraLotusAA *func_result = getPtGraph(func);

        if (!func_result)
          continue;

        // Get new call graph resolution results
        const auto &newCgResults = func_result->cg_resolve_result;

        // Update function pointer results and detect changes
        for (const auto &callsiteResult : newCgResults) {
          Value *callsite = callsiteResult.first;
          const CallTargetSet &newTargets = callsiteResult.second;

          CallTargetSet *oldTargets =
              functionPointerResults_.getTargets(func, callsite);

          // Target membership, not path-condition deltas, drives
          // fixpoint iteration. Conditions are still updated in the database.
          bool targetsChanged = !hasSameTargetMembership(oldTargets, newTargets);

          if (targetsChanged) {
            changed_func.insert(func);
            changed = true;
          }

          // Update targets
          functionPointerResults_.setTargets(func, callsite, newTargets);

          // Update call graph edges
          for (const auto &target_item : newTargets) {
            callGraphState_.addEdge(func, target_item.first);
          }
        }
      }

      // Detect back edges in updated call graph
      callGraphState_.detectBackEdges(changed_func);

      if (!changed_func.empty())
        changed = true;
    } else {
      break; // No CG updates, single iteration
    }

    if (!changed) {
      changed = true;
      iteration = lotus_restrict_cg_iter;
      continue;
    }

    iteration++;
  }

  outs() << "[LotusAA] Analysis complete\n";
}

void LotusAA::finalizeCg(std::vector<Function *> &func_seq) {
  if (lotus_print_cg) {
    for (Function *func : func_seq) {
      IntraLotusAA *result = getPtGraph(func);
      if (result) {
        result->showFunctionPointers();
      }
    }
  }

  if (lotus_print_pts) {
    for (Function *func : func_seq) {
      IntraLotusAA *result = getPtGraph(func);
      if (result) {
        result->show();
      }
    }
  }
}

bool LotusAA::computePTA(Function *F) {
  assert(intraResults_.count(F));

  IntraLotusAA *old_result = intraResults_[F];
  IntraLotusAA *new_result = new IntraLotusAA(F, this);

  new_result->computePTA();

  if (lotus_cg)
    new_result->computeCG();

  if (old_result)
    delete old_result;

  intraResults_[F] = new_result;
  // Conservative behavior: a recomputed function is treated as having
  // potentially changed interface semantics.
  return true;
}

IntraLotusAA *LotusAA::getPtGraph(Function *F) {
  auto it = intraResults_.find(F);
  return (it == intraResults_.end()) ? nullptr : it->second;
}

DominatorTree *LotusAA::getDomTree(Function *F) {
  std::lock_guard<std::mutex> lock(domMutex_);

  // Check if already computed
  auto it = dominatorTrees_.find(F);
  if (it != dominatorTrees_.end())
    return it->second;

  // External functions (declarations) have no body, so no dominator tree
  if (F->isDeclaration()) {
    dominatorTrees_[F] = nullptr;
    return nullptr;
  }

  // Compute dominator tree for this function
  DominatorTree *DT = new DominatorTree(*F);
  dominatorTrees_[F] = DT;
  return DT;
}

bool LotusAA::isBackEdge(Function *caller, Function *callee) {
  return callGraphState_.isBackEdge(caller, callee);
}

CallTargetSet *LotusAA::getCallees(Function *func, Value *callsite) {
  return functionPointerResults_.getTargets(func, callsite);
}

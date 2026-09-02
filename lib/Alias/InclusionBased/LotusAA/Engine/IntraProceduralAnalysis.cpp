/// @file IntraProceduralAnalysis.cpp
/// @brief Main driver for intra-procedural pointer analysis in LotusAA
///
/// This file contains the **analysis orchestration** logic that coordinates all
/// transfer functions to perform flow-sensitive, field-sensitive pointer
/// analysis within a single function.
///
/// **Architecture:**
/// ```
/// IntraLotusAA (per-function analysis)
///   ├── computePTA() - Main analysis driver
///   │   ├── Process instructions in topological order
///   │   ├── Dispatch to transfer functions by opcode
///   │   └── Collect function interface (summary)
///   ├── computeCG() - Call graph resolution
///   └── Analysis utilities (show, clearMemory, etc.)
/// ```
///
/// **Transfer Function Organization** (in TransferFunctions/ subdirectory):
/// - `PointerInstructions.cpp`: Load, Store, PHI, Select, GEP, Casts,
/// processBasePointer
/// - `BasicOps.cpp`: Alloca, Arguments, Globals, Constants
/// - `CallHandling.cpp`: Function calls and summary application
/// - `CallGraphSolver.cpp`: Indirect call resolution
/// - `SummaryBuilder.cpp`: Function summary collection
///
/// **Analysis Phases:**
/// 1. **Initialization**: Topological BB ordering, sequence numbering
/// 2. **Instruction Processing**: Dispatch by opcode to transfer functions
/// 3. **Summary Generation**: Extract inputs/outputs/escaped objects
/// 4. **Call Graph Construction**: Resolve indirect calls (if enabled)
///
/// **Configuration Options:**
/// - `lotus_restrict_inline_depth`: Max inter-procedural inlining depth
///   (default: unbounded, Falcon-compatible sentinel `-2`)
/// - `lotus_restrict_cg_size`: Max indirect call targets (default: 5)
/// - `lotus_restrict_inline_size`: Max summary size (default: 100)
/// - `lotus_restrict_ap_level`: Max access path depth (default: 2)
///
/// @see IntraProceduralAnalysis.h for class declaration and data structures
/// @see TransferFunctions/ subdirectory for individual transfer function
/// implementations

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"

#include "Alias/InclusionBased/LotusAA/Support/Config.h"

#include <deque>
#include <unordered_map>

#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

namespace {

static const int LOTUS_INLINE_LEVEL_UNDEFINED = -2;

static void topSortCFG(std::vector<BasicBlock *> &bb_sorted, Function *F) {
  unordered_map<BasicBlock *, int> in_deg;
  deque<BasicBlock *> worklist;

  for (BasicBlock &bb_ref : *F) {
    BasicBlock *bb = &bb_ref;
    int num = 0;
    for (pred_iterator PI = pred_begin(bb), PE = pred_end(bb); PI != PE; ++PI) {
      ++num;
    }
    in_deg[bb] = num;
  }

  for (BasicBlock &bb_ref : *F) {
    BasicBlock *bb = &bb_ref;
    if (in_deg[bb] == 0)
      worklist.push_back(bb);
  }

  while (!worklist.empty()) {
    BasicBlock *bb = worklist.front();
    worklist.pop_front();

    for (succ_iterator I = succ_begin(bb), E = succ_end(bb); I != E; ++I) {
      BasicBlock *succ_bb = *I;
      if (--in_deg[succ_bb] == 0)
        worklist.push_back(succ_bb);
    }

    bb_sorted.push_back(bb);
  }
}

} // namespace

// Configuration
int IntraLotusAAConfig::lotus_restrict_inline_depth =
    LOTUS_INLINE_LEVEL_UNDEFINED;
int IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
double IntraLotusAAConfig::lotus_timeout = 10.0;
int IntraLotusAAConfig::lotus_restrict_cg_size = 5;
int IntraLotusAAConfig::pts_setting = 0;
bool IntraLotusAAConfig::lotus_test_correctness = false;
bool IntraLotusAAConfig::lotus_disable_library_heuristic = false;
bool IntraLotusAAConfig::lotus_disable_thread_heuristic = false;
bool IntraLotusAAConfig::lotus_use_valuetostring = false;
int IntraLotusAAConfig::lotus_restrict_inline_size = 100;
int IntraLotusAAConfig::lotus_restrict_ap_level = 2;
bool IntraLotusAAConfig::lotus_use_full_phi_cond = false;
bool IntraLotusAAConfig::lotus_enable_score_computation = false;
bool IntraLotusAAConfig::lotus_enable_summary_value = false;
bool IntraLotusAAConfig::lotus_enable_must_kill = true;
int IntraLotusAAConfig::lotus_restrict_output_pts = 10;
int IntraLotusAAConfig::lotus_memory_max_passing_func = 50;
int IntraLotusAAConfig::lotus_restrict_right_value_count = 100;
int IntraLotusAAConfig::lotus_restrict_inter_structure = -1;

static cl::opt<int> lotus_restrict_inline_depth_cl(
    "lotus-restrict-inline-depth",
    cl::desc("Maximum inlining depth for inter-procedural analysis"),
    cl::init(LOTUS_INLINE_LEVEL_UNDEFINED), cl::Hidden);

static cl::opt<int> lotus_restrict_cg_size_cl(
    "lotus-restrict-cg-size",
    cl::desc("Maximum indirect call targets to process"), cl::init(5),
    cl::Hidden);

static cl::opt<int> lotus_restrict_summary_ap_depth_cl(
    "lotus-restrict-summary-ap-depth",
    cl::desc("Restrict the AP-depth of summary nodes for interfaces"),
    cl::init(10), cl::Hidden);

static cl::opt<double>
    lotus_timeout_cl("lotus-timeout",
                     cl::desc("Restrict per-function LotusAA time in seconds"),
                     cl::init(10.0), cl::Hidden);

static cl::opt<int>
    lotus_pts_setting_cl("lotus-pts-setting",
                         cl::desc("Set LotusAA points-to choice"), cl::init(0),
                         cl::Hidden);

static cl::opt<bool> lotus_disable_thread_heuristic_cl(
    "lotus-disable-thread-heuristic",
    cl::desc("Disable thread heuristic processing in LotusAA"), cl::init(false),
    cl::Hidden);

static cl::opt<bool> lotus_disable_library_heuristic_cl(
    "lotus-disable-library-heuristic",
    cl::desc(
        "Disable heuristic processing of unknown library calls in LotusAA"),
    cl::init(false), cl::Hidden);

static cl::opt<bool> lotus_use_full_phi_cond_cl(
    "lotus-use-full-phi-cond",
    cl::desc("Use full unit block conditions for phi guards"), cl::init(false),
    cl::Hidden);

static cl::opt<bool> lotus_enable_score_computation_cl(
    "lotus-enable-score-computation",
    cl::desc("Enable call barrier confidence computation"), cl::init(false),
    cl::Hidden);

static cl::opt<bool>
    lotus_enable_summary_value_cl("lotus-enable-summary-value",
                                  cl::desc("Emit summary-value memory effects"),
                                  cl::init(false), cl::Hidden);

static cl::opt<bool> lotus_enable_must_kill_cl(
    "lotus-enable-must-kill",
    cl::desc("Use Tuna-style must-kill forests for load-store matching"),
    cl::init(true), cl::Hidden);

static cl::opt<int> lotus_restrict_output_pts_cl(
    "lotus-restrict-output-pts",
    cl::desc("Restrict pseudo output points-to entries"), cl::init(10),
    cl::Hidden);

static cl::opt<int> lotus_memory_max_passing_func_cl(
    "lotus-restrict-memory-max-passing-func",
    cl::desc("Maximum calls considered by score computation"), cl::init(50),
    cl::Hidden);

static cl::opt<int> lotus_restrict_right_value_count_cl(
    "lotus-restrict-right-value-count",
    cl::desc("Maximum number of right values a value can keep"), cl::init(100),
    cl::Hidden);

static cl::opt<int> lotus_restrict_inter_structure_cl(
    "lotus-restrict-inter-structure",
    cl::desc("Merge escaped recursive/list/tree-like structures in LotusAA "
             "(-1 disables the heuristic)"),
    cl::init(-1), cl::Hidden);

void IntraLotusAAConfig::setParam() {
  if (lotus_restrict_inline_depth_cl.getNumOccurrences() > 0)
    lotus_restrict_inline_depth = lotus_restrict_inline_depth_cl;
  if (lotus_restrict_cg_size_cl.getNumOccurrences() > 0)
    lotus_restrict_cg_size = lotus_restrict_cg_size_cl;
  if (lotus_restrict_summary_ap_depth_cl.getNumOccurrences() > 0)
    lotus_restrict_summary_ap_depth = lotus_restrict_summary_ap_depth_cl;
  if (lotus_timeout_cl.getNumOccurrences() > 0)
    lotus_timeout = lotus_timeout_cl;
  if (lotus_pts_setting_cl.getNumOccurrences() > 0)
    pts_setting = lotus_pts_setting_cl;
  if (lotus_disable_library_heuristic_cl.getNumOccurrences() > 0)
    lotus_disable_library_heuristic = lotus_disable_library_heuristic_cl;
  if (lotus_disable_thread_heuristic_cl.getNumOccurrences() > 0)
    lotus_disable_thread_heuristic = lotus_disable_thread_heuristic_cl;
  if (lotus_use_full_phi_cond_cl.getNumOccurrences() > 0)
    lotus_use_full_phi_cond = lotus_use_full_phi_cond_cl;
  if (lotus_enable_score_computation_cl.getNumOccurrences() > 0)
    lotus_enable_score_computation = lotus_enable_score_computation_cl;
  if (lotus_enable_summary_value_cl.getNumOccurrences() > 0)
    lotus_enable_summary_value = lotus_enable_summary_value_cl;
  if (lotus_enable_must_kill_cl.getNumOccurrences() > 0)
    lotus_enable_must_kill = lotus_enable_must_kill_cl;
  if (lotus_restrict_output_pts_cl.getNumOccurrences() > 0)
    lotus_restrict_output_pts = lotus_restrict_output_pts_cl;
  if (lotus_memory_max_passing_func_cl.getNumOccurrences() > 0)
    lotus_memory_max_passing_func = lotus_memory_max_passing_func_cl;
  if (lotus_restrict_right_value_count_cl.getNumOccurrences() > 0)
    lotus_restrict_right_value_count = lotus_restrict_right_value_count_cl;
  if (lotus_restrict_inter_structure_cl.getNumOccurrences() > 0)
    lotus_restrict_inter_structure = lotus_restrict_inter_structure_cl;
}

// IntraLotusAA implementation
const int IntraLotusAA::PTR_TO_ESC_OBJ = -1;

IntraLotusAA::IntraLotusAA(Function *F, LotusAA *lotus_aa)
    : PTGraph(F, lotus_aa), func_obj(nullptr), func_new(nullptr),
      is_PTA_computed(false), is_CG_computed(false),
      is_considered_as_library(false), is_timeout_found(false),
      inline_ap_depth(0), pts_setting(IntraLotusAAConfig::pts_setting),
      timer(nullptr) {
  getReturnInst();

  if (IntraLotusAAConfig::lotus_restrict_summary_ap_depth < 0) {
    IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 0;
  } else if (IntraLotusAAConfig::lotus_restrict_summary_ap_depth >
             ::LotusConfig::MAXIMAL_SUMMARY_AP_DEPTH) {
    IntraLotusAAConfig::lotus_restrict_summary_ap_depth =
        ::LotusConfig::MAXIMAL_SUMMARY_AP_DEPTH;
  }

  for (int i = 0; i <= IntraLotusAAConfig::lotus_restrict_summary_ap_depth;
       i++) {
    summary_inputs.push_back(new set<Value *, llvm_cmp>);
    summary_outputs.push_back(new mem_value_t);
  }

  topSortCFG(topBBs, F);
}

IntraLotusAA::~IntraLotusAA() {
  for (OutputItem *item : outputs) {
    delete item;
  }

  // Delete pseudo-argument Argument objects created in createPseudoOutputNodes
  // and createEscapedObjects.  These are synthetic LLVM Argument objects that
  // are NOT owned by any LLVM Function, so they must be freed here.
  // Previously they were only stored in func_pseudo_ret_cache (as keys in
  // pt_results and as values in func_ret) but never deleted, causing a leak
  // proportional to the number of call sites × callees.
  for (auto &kv : func_pseudo_ret_cache) {
    // kv.first is the synthetic Argument* we allocated with `new
    // Argument(...)`. It is safe to delete because it has no parent Function.
    kv.first->deleteValue();
  }
  func_pseudo_ret_cache.clear();

  if (func_new)
    func_new->deleteValue();

  for (mem_value_t *vals : summary_outputs) {
    delete vals;
  }
  for (set<Value *, llvm_cmp> *vals : summary_inputs) {
    delete vals;
  }

  if (timer)
    delete timer;
}

void IntraLotusAA::computePTA() {
  if (is_considered_as_library || is_PTA_computed)
    return;

  setTimer((unsigned)IntraLotusAAConfig::lotus_timeout);

  // Cache instruction sequence
  int seq_num = 0;
  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      value_seq[&inst] = seq_num++;
    }
  }

  cacheFunctionCallInfo();

  // Process instructions
  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      if (is_timeout_found)
        return;
      if (timer)
        timer->check();
      if (is_timeout_found)
        return;

      switch (inst.getOpcode()) {
      case Instruction::Store:
        processStore(cast<StoreInst>(&inst));
        break;

      case Instruction::Load: {
        LoadInst *load = cast<LoadInst>(&inst);
        if (load->getType()->isPointerTy())
          processLoad(load);
        else {
          mem_value_t tmp;
          processBasePointer(load->getPointerOperand());
          collectPathSensitiveLoadValues(load, tmp, true);
        }
        break;
      }

      case Instruction::PHI:
        if (inst.getType()->isPointerTy())
          processPhi(cast<PHINode>(&inst));
        break;

      case Instruction::Alloca:
        processAlloca(cast<AllocaInst>(&inst));
        break;

      case Instruction::Call:
      case Instruction::Invoke:
        if (!isa<DbgInfoIntrinsic>(&inst)) {
          processCall(cast<CallBase>(&inst));
        }
        break;

      case Instruction::Select:
        if (inst.getType()->isPointerTy())
          processSelect(cast<SelectInst>(&inst));
        break;

      case Instruction::BitCast:
      case Instruction::GetElementPtr:
        processBasePointer(&inst);
        break;
      }
    }
  }

  // Collect interface for interprocedural analysis
  if (IntraLotusAAConfig::lotus_restrict_inline_depth != 0) {
    collectOutputs();
    collectInputs();
    finalizeInterface();
  }

  is_PTA_computed = true;
}

void IntraLotusAA::show() {
  outs() << "\n========== LotusAA Results: " << analyzed_func->getName()
         << " ==========\n";

  // Show points-to sets
  for (auto &it : pt_results) {
    Value *ptr = it.first;
    if (!ptr)
      continue;

    PTResult *res = it.second;
    PTResultIterator iter(res, this);

    outs() << "Pointer: ";
    if (ptr->hasName())
      outs() << ptr->getName();
    else
      ptr->print(outs());
    outs() << " -> " << iter.size() << " locations\n";
    outs() << iter << "\n";
  }

  outs() << "==============================================\n\n";
}

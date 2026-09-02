/*
 * LotusAA - Function-Level Pointer Analysis
 *
 * Flow-sensitive, field-sensitive intra-procedural pointer analysis.
 * This is the core analysis engine that processes individual functions.
 *
 * Key Responsibilities:
 * - Process LLVM instructions to build points-to graph
 * - Generate function summaries (inputs/outputs/escaped objects)
 * - Track field-sensitive memory objects
 * - Support inter-procedural analysis via summaries
 */

/// @file IntraProceduralAnalysis.h
/// @brief Function-level pointer analysis — the core per-function engine
///
/// `IntraLotusAA` extends `PTGraph` with instruction-level transfer functions
/// and interprocedural summary generation.  It processes one function at a
/// time in topological block order.
///
/// ## Configuration (`IntraLotusAAConfig`)
///
/// Global settings controlling inlining depth, access-path depth, timeout,
/// call-graph size, memory-tracking limits, and heuristic flags.
///
/// ## Analysis phases (`computePTA`)
///
/// 1. **Sequence numbering** — assigns a monotonic index to every instruction
///    for dominance-based value versioning.
/// 2. **Transfer functions** — dispatches each instruction to a specialised
///    handler (processAlloca, processStore, processLoad, processPhi,
///    processCall, processSelect, processGepBitcast, processCast,
///    processBasePointer).
/// 3. **Summary generation** — collects inputs (symbolic arguments +
///    side-effect inputs), outputs (return value + side-effect outputs), and
///    escaped objects for interprocedural analysis.
///
/// ## Key data structures
///
///   - `inputs` / `outputs` / `escape_objs` — function interface
///   - `func_arg` — per-callsite / per-callee argument bindings
///   - `func_ret` — per-callsite / per-callee return-value pseudo-arguments
///   - `func_pseudo_ret_cache` — origin (callsite, index) of each pseudo-return
///   - `cg_resolve_result` / `output_cg_summary` / `input_cg_summary` — CG data
///   - `ret_insts` — return instructions with path conditions
///
/// ## Access Path
///
/// `AccessPath` encodes `(parent Value *, offset)` pairs representing
/// field dereferences (e.g., `ptr->a->b` becomes a chain of AccessPath
/// links).  Used to model side-effect inputs/outputs in function summaries.

#pragma once

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/MemoryModel/PointsToGraph.h"
#include "Alias/InclusionBased/LotusAA/MemoryModel/Types.h"
#include "Utils/Platform/Timer.h"

#include <list>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

namespace llvm {

/*
 * Global configuration for LotusAA
 */
/// Global configuration for LotusAA.  Static fields are initialised from
/// command-line options; `setParam()` reconciles them after parsing.
class IntraLotusAAConfig {
public:
  static int lotus_restrict_inline_depth;
  static int lotus_restrict_summary_ap_depth;
  static double lotus_timeout;
  static int lotus_restrict_cg_size;
  static int pts_setting;
  static bool lotus_test_correctness;
  static bool lotus_disable_library_heuristic;
  static bool lotus_disable_thread_heuristic;
  static bool lotus_use_valuetostring;
  static int lotus_restrict_inline_size;
  static int lotus_restrict_ap_level;
  static bool lotus_use_full_phi_cond;
  static bool lotus_enable_score_computation;
  static bool lotus_enable_summary_value;
  static bool lotus_enable_must_kill;
  static int lotus_restrict_output_pts;
  static int lotus_memory_max_passing_func;
  static int lotus_restrict_right_value_count;
  static int lotus_restrict_inter_structure;

  static void setParam();
};

/*
 * IntraLotusAA - Intra-procedural pointer analysis
 */
/// Per-function pointer analysis engine.
///
/// Instantiated once per function by the interprocedural pass.  Provides all
/// instruction-level transfer functions and collects function summaries for
/// interprocedural propagation.
class IntraLotusAA : public PTGraph {
public:
  PTGType getKind() const override { return IntraLotusAATy; }

  static bool classof(const PTGraph *G) {
    return G->getKind() == IntraLotusAATy;
  }

  // Symbolic access path: parent->offset
  class AccessPath {
    Value *parent;
    int64_t offset;

  public:
    AccessPath(Value *parent = nullptr, int64_t offset = 0)
        : parent(parent), offset(offset) {}
    AccessPath(const AccessPath &info)
        : parent(info.parent), offset(info.offset) {}
    AccessPath &operator=(const AccessPath &info) {
      parent = info.parent;
      offset = info.offset;
      return *this;
    }

    void reset(Value *p = nullptr, int64_t off = 0) {
      parent = p;
      offset = off;
    }

    int64_t getOffset() const { return offset; }
    Value *getParentPtr() const { return parent; }

    friend class IntraLotusAA;
  };

  // Function output
  class OutputItem {
    AccessPath symbolic_info;
    std::map<ReturnInst *, mem_value_t, llvm_cmp> val;
    Type *output_ty;
    std::list<std::pair<path_cond_t, AccessPath>> pseudo_pts;
    int func_level;

  public:
    AccessPath &getSymbolicInfo() { return symbolic_info; }
    const AccessPath &getSymbolicInfo() const { return symbolic_info; }
    std::list<std::pair<path_cond_t, AccessPath>> &getPseudoPointTo() {
      return pseudo_pts;
    }
    const std::list<std::pair<path_cond_t, AccessPath>> &getPseudoPointTo() const {
      return pseudo_pts;
    }
    std::map<ReturnInst *, mem_value_t, llvm_cmp> &getVal() { return val; }
    const std::map<ReturnInst *, mem_value_t, llvm_cmp> &getVal() const {
      return val;
    }
    void setType(Type *type) { output_ty = type; }
    Type *getType() const { return output_ty; }
    int getFuncLevel() const { return func_level; }

    friend class IntraLotusAA;
  };

private:
  using func_arg_t = std::map<Value *, mem_value_t, llvm_cmp>;
  using cg_result_t = std::map<Function *, path_cond_t, llvm_cmp>;

  struct MustKillForest {
    LoadInst *anchor = nullptr;
    std::set<Instruction *, llvm_cmp> stores;
    std::set<Instruction *, llvm_cmp> roots;
  };

  // Function interface
  std::map<Value *, AccessPath, llvm_cmp> inputs;
  std::map<Value *, int, llvm_cmp> inputs_func_level;
  std::vector<OutputItem *> outputs;
  std::set<MemObject *, mem_obj_cmp> escape_objs;
  std::set<Value *, llvm_cmp> escape_source;
  std::map<Value *, unsigned, llvm_cmp> pseudo_input_indices;

  // Return instructions
  std::map<ReturnInst *, path_cond_t, llvm_cmp> ret_insts;

  // Call information
  std::map<Value *, std::map<Function *, func_arg_t, llvm_cmp>, llvm_cmp>
      func_arg;
  std::map<Value *, func_arg_t, llvm_cmp> thread_arg;
  std::map<Instruction *, std::map<Function *, std::vector<Value *>, llvm_cmp>,
           llvm_cmp>
      func_ret;
  std::map<Value *, std::pair<Instruction *, int>, llvm_cmp>
      func_pseudo_ret_cache;

  // CG resolution
  std::map<Value *, cg_result_t, llvm_cmp> cg_resolve_result;

  // CG summaries
  std::vector<cg_result_t> output_cg_summary;
  std::map<Argument *, std::map<cg_result_t *, path_cond_t>, llvm_cmp>
      input_cg_summary;

  // Escaped object mapping
  using escapedMap = std::map<MemObject *, MemObject *, mem_obj_cmp>;
  std::map<Value *, std::map<Function *, escapedMap, llvm_cmp>, llvm_cmp>
      func_escape;

  // Pseudo objects for merging
  std::map<MemObject *, MemObject *, mem_obj_cmp> real_to_pseudo_map;
  std::map<MemObject *, std::set<MemObject *, mem_obj_cmp>, mem_obj_cmp>
      pseudo_to_real_map;

  // Access path tracking for escaped objects
  std::map<Value *, std::pair<AccessPath, int64_t>, llvm_cmp> escape_obj_path;
  std::map<Value *, std::pair<Value *, int64_t>, llvm_cmp> escape_ret_path;

  // Topological BB order
  std::vector<BasicBlock *> topBBs;

  // Special objects
  MemObject *func_obj;
  Argument *func_new;
  std::vector<mem_value_t *> summary_outputs;
  std::vector<std::set<Value *, llvm_cmp> *> summary_inputs;
  std::map<Value *, int, llvm_cmp> summary_inputs_idx;

  // Value sequence
  std::map<Value *, int, llvm_cmp> value_seq;

  // Tuna-style staged strong-update state.  Forests are cached at loads and
  // inherited from an immediate dominating must-alias load.
  std::map<LoadInst *, MustKillForest, llvm_cmp> must_kill_forests;
  std::map<std::pair<const Instruction *, const Instruction *>, bool>
      reachability_cache;
  std::map<std::tuple<const Instruction *, const Instruction *,
                      const Instruction *>,
           bool>
      avoiding_reachability_cache;

  // Flags
  bool is_PTA_computed;
  bool is_CG_computed;
  bool is_considered_as_library;
  bool is_timeout_found;

  int inline_ap_depth;
  int &pts_setting;
  Timer *timer;

  // Index for escaped object pointers
  static const int PTR_TO_ESC_OBJ;

private:
  // Instruction processors
  PTResult *processPhi(PHINode *phi);
  void processLoad(LoadInst *load);
  void processStore(StoreInst *store);
  void processCall(CallBase *call);
  PTResult *processAlloca(AllocaInst *alloca);
  PTResult *processSelect(SelectInst *select);
  PTResult *processArg(Argument *arg);
  PTResult *processGlobal(GlobalValue *global);
  PTResult *processNullptr(ConstantPointerNull *null_ptr);
  PTResult *processNonPointer(Value *non_pointer_val);
  PTResult *processUnknown(Value *unknown_val);
  PTResult *processGepBitcast(Value *val);
  PTResult *processCast(CastInst *ptr);
  PTResult *processBasePointer(Value *val);

  void processUnknownLibraryCall(CallBase *call);

  void processCalleeInput(std::map<Value *, AccessPath, llvm_cmp> &callee_input,
                          std::map<Value *, int, llvm_cmp> &inputs_func_level,
                          std::vector<Value *> &real_args,
                          std::vector<Value *> &formal_args, CallBase *callsite,
                          func_arg_t &result, path_cond_t pre_cond = nullptr);

  void processCalleeOutput(std::vector<OutputItem *> &callee_output,
                           std::set<MemObject *, mem_obj_cmp> &callee_escape,
                           Instruction *callsite, Function *callee,
                           path_cond_t pre_cond = nullptr);

  // Helper functions for processCalleeOutput
  std::vector<Value *> &
  createPseudoOutputNodes(std::vector<OutputItem *> &callee_output,
                          Instruction *callsite, Function *callee);

  void createEscapedObjects(
      std::set<MemObject *, mem_obj_cmp> &callee_escape, Instruction *callsite,
      Function *callee,
      std::map<Value *, MemObject *, llvm_cmp> &escape_object_map);

  void linkOutputPointsToResults(
      OutputItem *output, Value *curr_output,
      std::map<Value *, MemObject *, llvm_cmp> &escape_object_map,
      func_arg_t &callee_func_arg, Instruction *callsite, Function *callee,
      std::set<PTResult *> &visited);

  void linkOutputValues(
      OutputItem *output, Value *curr_output, size_t idx,
      std::map<Value *, MemObject *, llvm_cmp> &escape_object_map,
      func_arg_t &callee_func_arg, Instruction *callsite,
      std::unordered_map<PTResult *, PTResultIterator> &pt_result_cache,
      path_cond_t pre_cond);

  void collectOutputs();
  void collectInputs();
  void finalizeInterface();
  void cacheFunctionCallInfo();

  void collectEscapedObjects(
      std::map<MemObject *, MemObject *, mem_obj_cmp> &real_to_pseudo_map,
      std::map<MemObject *, std::set<MemObject *, mem_obj_cmp>, mem_obj_cmp>
          &pseudo_to_real_map);

  void resolveCallValue(Value *val, cg_result_t &target, path_cond_t pre_cond);

  void collectPathSensitiveLoadValues(LoadInst *load, mem_value_t &result,
                                      bool create_symbol);
  MustKillForest &getOrCreateMustKillForest(LoadInst *load);
  LoadInst *findImmediateMustAliasAnchor(LoadInst *load);
  bool instructionReaches(const Instruction *source,
                          const Instruction *target,
                          const Instruction *avoiding = nullptr);
  bool mustKill(StoreInst *killer, StoreInst *killed, LoadInst *load);

public:
  using FuncArgBindingMap =
      std::map<Value *, std::map<Function *, func_arg_t, llvm_cmp>, llvm_cmp>;
  using ThreadArgBindingMap = std::map<Value *, func_arg_t, llvm_cmp>;
  using CallReturnBindingMap =
      std::map<Instruction *, std::map<Function *, std::vector<Value *>, llvm_cmp>,
               llvm_cmp>;
  using PseudoReturnOriginMap =
      std::map<Value *, std::pair<Instruction *, int>, llvm_cmp>;
  using CallResolutionMap = std::map<Value *, cg_result_t, llvm_cmp>;

  struct StrongUpdateStats {
    LoadInst *anchor = nullptr;
    unsigned candidate_store_count = 0;
    unsigned root_store_count = 0;
  };

  IntraLotusAA(Function *F, LotusAA *lotus_aa);
  ~IntraLotusAA();

  // Main analysis methods
  void computePTA();
  void computeCG();

  // Utilities
  void show();
  void showFunctionPointers();
  bool isPure();
  bool isPseudoInput(Value *val);
  bool isSameInterface(IntraLotusAA *to_compare);
  bool isConsideredAsLibrary() const { return is_considered_as_library; }

  int getSequenceNum(Value *val) override;
  int getInlineApDepth() override;
  PTGraph *getPtGraph(Function *F) override;

  std::map<Value *, AccessPath, llvm_cmp> &getInputs() { return inputs; }
  const std::map<Value *, AccessPath, llvm_cmp> &getInputs() const {
    return inputs;
  }
  std::vector<OutputItem *> &getOutputs() { return outputs; }
  const std::vector<OutputItem *> &getOutputs() const { return outputs; }
  std::set<MemObject *, mem_obj_cmp> &getEscapeObjs() { return escape_objs; }
  const std::set<MemObject *, mem_obj_cmp> &getEscapeObjs() const {
    return escape_objs;
  }
  const FuncArgBindingMap &getCallArgBindings() const { return func_arg; }
  const ThreadArgBindingMap &getThreadArgBindings() const { return thread_arg; }
  const CallReturnBindingMap &getCallReturnBindings() const { return func_ret; }
  const PseudoReturnOriginMap &getPseudoReturnOrigins() const {
    return func_pseudo_ret_cache;
  }
  const CallResolutionMap &getResolvedCallTargets() const {
    return cg_resolve_result;
  }
  const std::vector<mem_value_t *> &getSummaryOutputs() const {
    return summary_outputs;
  }
  const std::vector<std::set<Value *, llvm_cmp> *> &getSummaryInputs() const {
    return summary_inputs;
  }
  void collectGuardedValueFlowLoadValues(LoadInst *load, mem_value_t &result);
  StrongUpdateStats getStrongUpdateStats(LoadInst *load);
  void collectGuardedValueFlowCallsiteSummaryInputs(
      CallBase *call, std::vector<mem_value_t> &summary_values);
  int getPseudoInputIndex(Value *value) const {
    auto it = pseudo_input_indices.find(value);
    return it == pseudo_input_indices.end() ? -1
                                            : static_cast<int>(it->second);
  }
  int getSummaryInputIndex(Value *value) const {
    auto it = summary_inputs_idx.find(value);
    return it == summary_inputs_idx.end() ? -1 : it->second;
  }
  path_cond_t importSummaryCond(path_cond_t cond, Value *callsite,
                                Function *callee) {
    return importPathCond(cond, callsite, callee);
  }

  void getReturnInst();

  // Access path utilities
  int getArgLevel(AccessPath &path);
  void getFullAccessPath(Value *target_val,
                         std::vector<std::pair<Value *, int64_t>> &result,
                         bool *is_from_return = nullptr);
  void getFullAccessPath(AccessPath &ap,
                         std::vector<std::pair<Value *, int64_t>> &result,
                         bool *is_from_return = nullptr);
  void
  getFullOutputAccessPath(int output_index,
                          std::vector<std::pair<Value *, int64_t>> &result,
                          bool *is_from_return = nullptr);

  // Caller-callee object mapping
  void getCallerObj(Value *call, Function *callee, SymbolicMemObject *calleeObj,
                    std::vector<std::pair<MemObject *, int64_t>> &result);
  MemObject *getCallerEscapeObj(Value *call, Function *callee,
                                MemObject *calleeObj);

  // Memory cleanup
  void clearIntermediatePtsResult();
  void clearIntermediateCgResult();
  void clearGlobalCgResult();
  void clearMemObjectResult();
  void clearInterfaceResult();

  // Interface check
  bool isPseudoInterface(Value *target);

  void onTimeOut();
  void setTimer(unsigned duration);

  friend class LotusAA;
};

} // namespace llvm

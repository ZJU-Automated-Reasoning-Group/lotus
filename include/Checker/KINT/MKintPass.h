/** @file MKintPass.h @brief LLVM pass wrapper for MKint integer analysis. */
#pragma once

#include "Checker/KINT/BugDetection.h"
#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/KINT/KINTTaintAnalysis.h"
#include "Checker/KINT/SmtMemory.h"
#include "Checker/KINT/SummaryEncoding.h"

#include <chrono>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Casting.h>
#include <z3++.h>

namespace kint {

// NOTE: 'using namespace llvm' is scoped inside the kint namespace to limit
// pollution.  It still affects all code inside this namespace block in every
// TU that includes this header, but it does not leak into the global namespace.
// The proper fix is to qualify every LLVM type with llvm:: throughout this
// header, but that is a larger refactor deferred for now.
using namespace llvm; // NOLINT(google-build-using-namespace)

struct MKintPass : public PassInfoMixin<MKintPass> {
  MKintPass();
  MKintPass(const MKintPass &) = delete;
  MKintPass &operator=(const MKintPass &) = delete;
  MKintPass(MKintPass &&) noexcept = default;
  MKintPass &operator=(MKintPass &&) noexcept = default;
  ~MKintPass() = default;

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

  // SARIF reporting
  void generateSarifReport(const std::string &filename);

private:
  // Bug reporting to BugReportMgr
  void reportBugsToManager();
  void reportBug(interr bug_type, const Instruction *inst,
                 const std::vector<PathPoint> &path = {});

  // Bug type IDs (registered with BugReportMgr)
  int m_intOverflowTypeId;
  int m_divByZeroTypeId;
  int m_badShiftTypeId;
  int m_arrayOOBTypeId;
  int m_deadBranchTypeId;
  void backedge_analysis(const Function &F);
  void smt_solving(Module &M);
  void path_solving(BasicBlock *cur, BasicBlock *pred);
  static std::string get_bb_label(const BasicBlock *bb);
  void buildSummaries(Module &M);
  const FunctionSummary *buildSummary(Function &F);
  SummaryAvailability applySummary(CallInst *call, BasicBlock *cur,
                                   BasicBlock *pred);
  bool recordSummaryCase(ReturnInst *ret, BasicBlock *cur, BasicBlock *pred);
  bool canSummarizeFunction(const Function &F) const;
  bool classifyPointerReturn(const Function &F, const Value *&root,
                             std::string &reason) const;
  bool isBoundaryVisiblePointerArg(const Argument &arg,
                                   const Function &F) const;
  std::vector<const GlobalVariable *>
  collectReferencedGlobals(const Function &F) const;
  std::vector<const Value *> collectSummaryObjects(const Function &F) const;
  bool collectModifiedBoundaryObjects(Function &F, FunctionSummary &summary);
  bool isAllocatorLike(const Function *callee) const;
  const Value *resolveAliasedObject(const Value *obj) const;
  const Value *commonPointerObject(const Value *a, const Value *b) const;
  const Value *getPointerOrigin(const Value *v) const;
  std::optional<z3::expr> getPointerOffset(const Value *v) const;
  void finalizeSummaryContract(FunctionSummary &summary);

  // SMT helpers (memory + symbol management)
  void pushSymFrame();
  void popSymFrame();
  void pushObjectFrame();
  void popObjectFrame();
  void setSym(const Value *v, const z3::expr &e);
  void pushConstraintFrame();
  void popConstraintFrame();
  void addConstraint(const z3::expr &e);
  z3::expr buildPathConstraintConjunction() const;
  bool checkBugCondition(const Instruction *inst, interr type,
                         const z3::expr &bugCond);
  void registerUniversal(const z3::expr &e);
  void dumpEfConstraint(const Instruction *inst, interr type,
                        const z3::expr &q) const;
  bool isRobustBugEnabled(interr type) const;
  void parseRobustBugFilter(const std::string &csv);
  const Value *getObjectForPtr(const Value *ptr) const;
  z3::expr loadBytesFromMem(const z3::expr &mem, const z3::expr &offset,
                            unsigned numBytes, bool littleEndian) const;
  z3::expr storeBytesToMem(const z3::expr &mem, const z3::expr &offset,
                           const z3::expr &value, unsigned numBytes,
                           bool littleEndian) const;
  void havocObject(const Value *obj, const std::string &hint);
  void havocObjectRange(const Value *obj, const z3::expr &offset,
                        uint64_t numBytes, const std::string &hint);
  void invalidateObject(const Value *obj, const std::string &hint);
  bool callMayModObject(llvm::CallBase *call, const Value *obj) const;
  z3::expr getValueExpr(const Value *v, BasicBlock *cur, BasicBlock *pred);
  z3::expr getIntExpr(const Value *v, BasicBlock *cur, BasicBlock *pred);
  z3::expr getPtrExpr(const Value *v, BasicBlock *cur, BasicBlock *pred);
  z3::expr gepOffsetBytes(const GetElementPtrInst *gep, BasicBlock *cur,
                          BasicBlock *pred);
  bool maybeCheckOOB(const Instruction *at, const Value *ptrOperand,
                     uint64_t accessBytes, BasicBlock *cur, BasicBlock *pred);
  bool addWellDefinedConstraints(BinaryOperator *op, BasicBlock *cur,
                                 BasicBlock *pred);
  void ensureObject(const Value *obj, const std::string &hintName,
                    const z3::expr &sizeBytes, bool sizeKnown);
  bool isLittleEndian() const;
  z3::expr conjunctSummaryExprs(const std::vector<z3::expr> &exprs) const;
  z3::expr disjunctSummaryExprs(const std::vector<z3::expr> &exprs) const;
  z3::expr closeSummaryClause(const FunctionSummary &summary,
                              const z3::expr &clause) const;

  // Data members
  MapVector<Function *, std::vector<CallInst *>> m_func2tsrc;
  SetVector<Function *> m_taint_funcs;
  DenseMap<const BasicBlock *, SetVector<const BasicBlock *>> m_backedges;
  SetVector<StringRef> m_callback_tsrc_fn;

  // Error checking
  std::map<ICmpInst *, bool> m_impossible_branches;
  std::set<GetElementPtrInst *> m_gep_oob;
  std::set<Instruction *> m_overflow_insts;
  std::set<Instruction *> m_bad_shift_insts;
  std::set<Instruction *> m_div_zero_insts;

  // SMT solving
  std::optional<z3::solver> m_solver;
  std::unique_ptr<SmtMemory> m_smt_mem;
  DenseMap<const Value *, std::optional<z3::expr>> m_v2sym;
  std::map<const BasicBlock *, SmallVector<BasicBlock *, 2>> m_bbpaths;
  std::chrono::time_point<std::chrono::steady_clock> m_function_start_time;
  unsigned m_function_timeout;
  unsigned m_path_limit;
  uint64_t m_paths_explored = 0;
  bool m_path_limit_hit = false;
  bool m_timeout_hit = false;
  bool m_summary_backedge_hit = false;
  bool m_robust_reachability = false;
  std::string m_dump_ef_path;
  bool m_robust_universal_unknown_loads = false;
  bool m_robust_universal_external_globals = false;
  bool m_robust_universal_inline_asm = false;
  std::set<interr> m_robust_bug_filter;
  std::vector<z3::expr> m_path_constraints;
  std::vector<size_t> m_constraint_frames;
  std::vector<z3::expr> m_universal_vars;
  std::unordered_set<Z3_ast> m_universal_var_ids;
  llvm::AAResults *m_aa = nullptr;
  llvm::MemorySSA *m_mssa = nullptr;
  llvm::FunctionAnalysisManager *m_fam = nullptr;

  // Memory/object abstraction for pointer reasoning (SMT only)
  const DataLayout *m_dl = nullptr;
  unsigned m_ptr_bits = 0;
  DenseMap<const Value *, std::optional<z3::expr>>
      m_obj_base; // base address for allocation-like objects
  DenseMap<const Value *, std::optional<z3::expr>>
      m_obj_size; // size in bytes (bit-vector, ptr width)
  std::vector<const Value *>
      m_obj_list; // stable iteration order for constraints
  DenseMap<const Value *, std::optional<z3::expr>>
      m_obj_mem; // per-object byte arrays
  DenseMap<const Value *, const Value *> m_obj_alias;
  DenseMap<const Value *, const Value *> m_int_alias;
  DenseMap<const Value *, std::optional<z3::expr>> m_ptr_offset;
  DenseMap<const Value *, std::optional<z3::expr>> m_int_offset;
  DenseSet<const Value *> m_obj_freed;
  struct ObjectStateFrame {
    DenseMap<const Value *, std::optional<z3::expr>> obj_base;
    DenseMap<const Value *, std::optional<z3::expr>> obj_size;
    std::vector<const Value *> obj_list;
    DenseMap<const Value *, std::optional<z3::expr>> obj_mem;
    DenseMap<const Value *, const Value *> obj_alias;
    DenseMap<const Value *, const Value *> int_alias;
    DenseMap<const Value *, std::optional<z3::expr>> ptr_offset;
    DenseMap<const Value *, std::optional<z3::expr>> int_offset;
    DenseSet<const Value *> obj_freed;
  };
  std::vector<ObjectStateFrame> m_object_frames;
  struct SymChange {
    const Value *key = nullptr;
    bool hadOld = false;
    std::optional<z3::expr> oldValue;
  };
  std::vector<SymChange> m_sym_change_log;
  std::vector<size_t> m_sym_change_frames;
  DenseMap<const Function *, SummaryCacheEntry> m_summary_cache;
  Module *m_module = nullptr;
  FunctionSummary *m_building_summary = nullptr;
  bool m_summary_failed = false;
  std::string m_summary_failure_reason;
  unsigned m_summary_timeout = 0;
  unsigned m_summary_path_limit = 0;

  // Analysis components
  std::unique_ptr<TaintAnalysis> m_taint_analysis;
  std::unique_ptr<BugDetection> m_bug_detection;
};

} // namespace kint

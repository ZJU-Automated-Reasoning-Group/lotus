#include "Checker/KINT/MKintPass.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>
#include <z3++.h>

using namespace llvm;

namespace kint {

namespace {

static z3::expr bvValFromAPInt(z3::context &ctx, const llvm::APInt &value) {
  llvm::SmallString<64> decimal;
  value.toString(decimal, 10, /*Signed=*/false, /*formatAsCLiteral=*/false);
  Z3_sort sort = Z3_mk_bv_sort(ctx, value.getBitWidth());
  Z3_ast ast = Z3_mk_numeral(ctx, decimal.c_str(), sort);
  return z3::to_expr(ctx, ast);
}

} // namespace

static const char *bugTypeToString(interr t) {
  switch (t) {
  case interr::NONE:
    return "none";
  case interr::INT_OVERFLOW:
    return "integer overflow";
  case interr::DIV_BY_ZERO:
    return "divide by zero";
  case interr::BAD_SHIFT:
    return "bad shift";
  case interr::ARRAY_OOB:
    return "array index out of bound";
  case interr::DEAD_TRUE_BR:
    return "impossible true branch";
  case interr::DEAD_FALSE_BR:
    return "impossible false branch";
  default:
    return "unknown";
  }
}

static llvm::Optional<uint64_t> getConstantU64(const llvm::Value *v) {
  if (!v)
    return llvm::None;
  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    return ci->getZExtValue();
  }
  return llvm::None;
}

static std::atomic<uint64_t> g_obj_mem_id{0};

static z3::expr boolToBv1(const z3::expr &b) {
  return z3::ite(b, b.ctx().bv_val(1, 1), b.ctx().bv_val(0, 1));
}

static bool
computeWithOverflow(const llvm::WithOverflowInst *woi, z3::solver &solver,
                    const std::function<z3::expr(const llvm::Value *)> &getInt,
                    z3::expr &outResult, z3::expr &outOverflowBool) {
  if (!woi)
    return false;
  auto lhs = getInt(woi->getArgOperand(0));
  auto rhs = getInt(woi->getArgOperand(1));

  switch (woi->getIntrinsicID()) {
  case llvm::Intrinsic::uadd_with_overflow:
    outResult = lhs + rhs;
    outOverflowBool = !z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/false);
    return true;
  case llvm::Intrinsic::usub_with_overflow:
    outResult = lhs - rhs;
    outOverflowBool = !z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/false);
    return true;
  case llvm::Intrinsic::umul_with_overflow:
    outResult = lhs * rhs;
    outOverflowBool = !z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/false);
    return true;
  case llvm::Intrinsic::sadd_with_overflow:
    outResult = lhs + rhs;
    outOverflowBool = (!z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/true) ||
                       !z3::bvadd_no_underflow(lhs, rhs));
    return true;
  case llvm::Intrinsic::ssub_with_overflow:
    outResult = lhs - rhs;
    outOverflowBool = (!z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/true) ||
                       !z3::bvsub_no_overflow(lhs, rhs));
    return true;
  case llvm::Intrinsic::smul_with_overflow:
    outResult = lhs * rhs;
    outOverflowBool = (!z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/true) ||
                       !z3::bvmul_no_underflow(lhs, rhs));
    return true;
  default:
    break;
  }
  outResult = solver.ctx().bv_val(0, 1);
  outOverflowBool = solver.ctx().bool_val(false);
  return false;
}

MKintPass::MKintPass()
    : m_solver(llvm::None), m_function_timeout(FunctionTimeout),
      m_path_limit(MaxPathsPerFunction) {
  m_range_analysis = std::make_unique<RangeAnalysis>();
  m_taint_analysis = std::make_unique<TaintAnalysis>();
  m_bug_detection = std::make_unique<BugDetection>();

  // Register bug types with BugReportMgr (shared pattern)
  BugReportMgr &mgr = BugReportMgr::get_instance();
  m_intOverflowTypeId =
      mgr.register_bug_type("Integer Overflow", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-190");
  m_divByZeroTypeId =
      mgr.register_bug_type("Divide by Zero", BugDescription::BI_MEDIUM,
                            BugDescription::BC_ERROR, "CWE-369");
  m_badShiftTypeId =
      mgr.register_bug_type("Bad Shift", BugDescription::BI_MEDIUM,
                            BugDescription::BC_ERROR, "Invalid shift amount");
  m_arrayOOBTypeId =
      mgr.register_bug_type("Array Out of Bounds", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-119, CWE-125");
  m_deadBranchTypeId =
      mgr.register_bug_type("Dead Branch", BugDescription::BI_LOW,
                            BugDescription::BC_ERROR, "Unreachable code");
}

void MKintPass::backedge_analysis(const Function &F) {
  // Compute true loop backedges using a DFS-based algorithm.
  // An edge pred->succ is a backedge iff succ dominates pred (i.e., succ is
  // an ancestor of pred in the DFS spanning tree).
  // We store, for each block B, the set of predecessors P such that P->B is
  // a backedge.  The existing usage is: m_backedges[cur].contains(pred).

  // Initialize all entries so every block has an (empty) set.
  for (const auto &bb_ref : F) {
    const auto *bb = &bb_ref;
    if (m_backedges.count(bb) == 0)
      m_backedges[bb] = {};
  }

  // DFS colouring: 0 = white (unvisited), 1 = grey (on stack), 2 = black
  // (done).
  DenseMap<const BasicBlock *, int> color;
  std::vector<std::pair<const BasicBlock *, bool>> stack; // (block, entered)
  stack.push_back(std::make_pair(&F.getEntryBlock(), false));

  while (!stack.empty()) {
    // C++14: no structured bindings; use .first/.second explicitly.
    const BasicBlock *bb = stack.back().first;
    const bool leaving = stack.back().second;
    stack.pop_back();

    if (leaving) {
      color[bb] = 2; // black
      continue;
    }

    if (color[bb] == 1)
      continue; // already on stack (cycle detected earlier)
    if (color[bb] == 2)
      continue; // already fully processed

    color[bb] = 1;                             // grey: on the DFS stack
    stack.push_back(std::make_pair(bb, true)); // push "leaving" marker

    for (const auto *succ : successors(bb)) {
      if (color[succ] == 1) {
        // succ is an ancestor in the DFS tree -> bb->succ is a backedge.
        m_backedges[succ].insert(bb);
      } else if (color[succ] == 0) {
        stack.push_back(std::make_pair(succ, false));
      }
    }
  }
}

PreservedAnalyses MKintPass::run(Module &M, ModuleAnalysisManager &MAM) {
  MKINT_LOG() << "Running MKint pass on module " << M.getName();

  // Apply the CheckAll flag if set to true
  if (CheckAll) {
    CheckIntOverflow = true;
    CheckDivByZero = true;
    CheckBadShift = true;
    CheckArrayOOB = true;
    CheckDeadBranch = true;
  }

  // Refresh performance options (they may change across runs).
  m_function_timeout = FunctionTimeout;
  m_path_limit = MaxPathsPerFunction;
  m_robust_reachability = RobustReachability;
  m_dump_ef_path = DumpEFConstraints;
  m_robust_universal_unknown_loads = RobustUniversalUnknownLoads;
  m_robust_universal_external_globals = RobustUniversalExternalGlobals;
  m_robust_universal_inline_asm = RobustUniversalInlineAsm;
  parseRobustBugFilter(RobustOnlyBugs);

  // Print checker configuration
  MKINT_LOG() << "Checker Configuration:";
  MKINT_LOG() << "  Integer Overflow: "
              << (CheckIntOverflow ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Division by Zero: "
              << (CheckDivByZero ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Bad Shift: " << (CheckBadShift ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Array Out of Bounds: "
              << (CheckArrayOOB ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Dead Branch: "
              << (CheckDeadBranch ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Robust Reachability: "
              << (m_robust_reachability ? "Enabled" : "Disabled");
  if (!m_dump_ef_path.empty()) {
    MKINT_LOG() << "  Dump EF Constraints: " << m_dump_ef_path;
  }
  if (m_robust_reachability) {
    MKINT_LOG() << "  Robust Universals: unknown-loads="
                << (m_robust_universal_unknown_loads ? "on" : "off")
                << ", external-globals="
                << (m_robust_universal_external_globals ? "on" : "off")
                << ", inline-asm="
                << (m_robust_universal_inline_asm ? "on" : "off");
    if (!m_robust_bug_filter.empty()) {
      MKINT_LOG() << "  Robust Bug Filter: custom list";
    }
  }

  // Warn if no checkers are enabled
  if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift &&
      !CheckArrayOOB && !CheckDeadBranch) {
    MKINT_WARN() << "No bug checkers are enabled. No bugs will be detected.";
    MKINT_WARN() << "Use --check-all=true or enable individual checkers with "
                    "--check-<checker-name>=true";
  }

  // FIXME: This is a hack.
  auto *ctx = new z3::context; // let it leak.
  m_solver = z3::solver(*ctx);
  m_dl = &M.getDataLayout();
  m_ptr_bits = m_dl->getPointerSizeInBits(0);
  m_smt_mem = std::make_unique<SmtMemory>(*ctx, m_ptr_bits);
  m_func2tsrc.clear();
  m_taint_funcs.clear();
  m_backedges.clear();
  m_callback_tsrc_fn.clear();
  m_func2range_info.clear();
  m_func2ret_range.clear();
  m_range_analysis_funcs.clear();
  m_global2range.clear();
  m_garr2ranges.clear();
  m_impossible_branches.clear();
  m_gep_oob.clear();
  m_overflow_insts.clear();
  m_bad_shift_insts.clear();
  m_div_zero_insts.clear();
  if (m_bug_detection) {
    m_bug_detection->clearState();
  }
  m_obj_base.clear();
  m_obj_size.clear();
  m_obj_list.clear();
  m_obj_mem.clear();
  m_bbpaths.clear();
  m_v2sym.clear();
  m_aa = nullptr;
  m_mssa = nullptr;
  m_fam = nullptr;
  m_sym_change_log.clear();
  m_sym_change_frames.clear();
  m_path_constraints.clear();
  m_constraint_frames.clear();
  m_universal_vars.clear();
  m_universal_var_ids.clear();

  // Mark taint sources.
  for (auto &F : M) {
    auto taint_sources = m_taint_analysis->get_taint_source(F);
    m_taint_analysis->mark_func_sinks(F, m_callback_tsrc_fn);
    if (TaintAnalysis::is_taint_src(F.getName()))
      m_func2tsrc[&F] = std::move(taint_sources);
  }

  // Propagate taint across functions
  m_taint_analysis->propagate_taint_across_functions(M, m_func2tsrc,
                                                     m_taint_funcs);

  // Also add main function to analysis if it exists and is not already in
  // taint_funcs
  for (auto &F : M) {
    if (!F.isDeclaration()) {
      backedge_analysis(F);
      // Add main function to analysis if it's not already there
      if (F.getName() == "main" && !m_taint_funcs.contains(&F)) {
        m_taint_funcs.insert(&F);
        MKINT_LOG() << "Added main function to analysis";
      }
    }
  }

  MKINT_LOG() << "Module after taint:";
  MKINT_LOG() << M;

  this->init_ranges(M);

  const DataLayout &DL = M.getDataLayout();
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  m_fam = &FAM;

  constexpr size_t max_try = 128;
  size_t try_count = 0;

  while (true) { // iterative range analysis.
    const auto old_fn_rng = m_func2range_info;
    const auto old_glb_rng = m_global2range;
    const auto old_glb_arrrng = m_garr2ranges;
    const auto old_fn_ret_rng = m_func2ret_range;

    for (auto *F : m_range_analysis_funcs) {
      llvm::AAResults *AA = nullptr;
      llvm::MemorySSA *MSSA = nullptr;
      if (auto *AARes = FAM.getCachedResult<llvm::AAManager>(*F)) {
        AA = AARes;
      } else {
        AA = &FAM.getResult<llvm::AAManager>(*F);
      }
      if (auto *MSSARes = FAM.getCachedResult<llvm::MemorySSAAnalysis>(*F)) {
        MSSA = &MSSARes->getMSSA();
      } else {
        MSSA = &FAM.getResult<llvm::MemorySSAAnalysis>(*F).getMSSA();
      }

      m_range_analysis->range_analysis(
          *F, m_func2range_info, m_backedges, m_global2range, m_garr2ranges,
          m_func2ret_range, m_impossible_branches, m_gep_oob, m_func2tsrc,
          m_callback_tsrc_fn, DL, AA, MSSA);
    }

    if (m_func2range_info == old_fn_rng && old_glb_rng == m_global2range &&
        old_fn_ret_rng == m_func2ret_range && old_glb_arrrng == m_garr2ranges)
      break;
    if (++try_count > max_try) {
      MKINT_LOG() << "[Iterative Range Analysis] "
                  << "Max try " << max_try << " reached, aborting.";
      break;
    }
  }

  this->print_all_ranges();

  this->smt_solving(M);

  m_bug_detection->mark_errors(m_impossible_branches, m_gep_oob,
                               m_overflow_insts, m_bad_shift_insts,
                               m_div_zero_insts);

  // Report bugs to BugReportMgr (shared pattern)
  reportBugsToManager();

  // Note: SARIF/JSON output is now handled centrally by BugReportMgr
  // in the tool driver, not by individual checkers

  return PreservedAnalyses::all();
}

void MKintPass::init_ranges(Module &M) {
  m_range_analysis->init_ranges(
      M, m_func2range_info, m_func2ret_range, m_range_analysis_funcs,
      m_global2range, m_garr2ranges, m_taint_funcs, m_callback_tsrc_fn);
}

void MKintPass::print_all_ranges() const {
  m_range_analysis->print_all_ranges(m_func2ret_range, m_global2range,
                                     m_garr2ranges, m_func2range_info,
                                     m_impossible_branches, m_gep_oob);
}

void MKintPass::smt_solving(Module &M) {
  for (auto *F : m_taint_funcs) {
    if (F->isDeclaration())
      continue;

    // Reset per-function SMT state (kept inside the solver push/pop).
    m_bbpaths.clear();
    m_v2sym.clear();
    m_smt_mem->reset();
    m_obj_base.clear();
    m_obj_size.clear();
    m_obj_list.clear();
    m_obj_mem.clear();
    m_sym_change_log.clear();
    m_sym_change_frames.clear();
    m_path_constraints.clear();
    m_constraint_frames.clear();
    m_universal_vars.clear();
    m_universal_var_ids.clear();
    if (m_bug_detection) {
      m_bug_detection->clearCurrentPath();
    }

    // Record start time for this function
    m_function_start_time = std::chrono::steady_clock::now();
    m_paths_explored = 0;
    m_path_limit_hit = false;
    MKINT_LOG() << "Beginning analysis of function " << F->getName();

    if (m_fam && (m_aa = m_fam->getCachedResult<llvm::AAManager>(*F))) {
      // cached
    } else if (m_fam) {
      m_aa = &m_fam->getResult<llvm::AAManager>(*F);
    } else {
      m_aa = nullptr;
    }
    if (m_fam) {
      if (auto *MSSARes = m_fam->getCachedResult<llvm::MemorySSAAnalysis>(*F)) {
        m_mssa = &MSSARes->getMSSA();
      } else {
        m_mssa = &m_fam->getResult<llvm::MemorySSAAnalysis>(*F).getMSSA();
      }
    } else {
      m_mssa = nullptr;
    }

    // Seed global objects (base address + size) so pointer arithmetic and
    // loads/stores have a model.
    for (auto &GV : M.globals()) {
      const uint64_t bytes = m_dl->getTypeAllocSize(GV.getValueType());
      ensureObject(&GV, ("global." + GV.getName()).str(),
                   m_solver.getValue().ctx().bv_val(bytes, m_ptr_bits),
                   /*sizeKnown=*/true);
    }

    // Seed stack objects (allocas) with constant sizes when possible.
    for (auto &bb : F->getBasicBlockList()) {
      for (auto &inst : bb) {
        if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
          const uint64_t elemBytes =
              m_dl->getTypeAllocSize(ai->getAllocatedType());
          z3::expr sizeBytesExpr =
              m_solver.getValue().ctx().bv_val(elemBytes, m_ptr_bits);
          bool known = true;
          if (ai->isArrayAllocation()) {
            if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
              sizeBytesExpr = m_solver.getValue().ctx().bv_val(
                  elemBytes * ci->getZExtValue(), m_ptr_bits);
            } else {
              auto countExpr =
                  getIntExpr(ai->getArraySize(), &F->getEntryBlock(), nullptr);
              const unsigned cbw = countExpr.get_sort().bv_size();
              if (cbw < m_ptr_bits)
                countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
              else if (cbw > m_ptr_bits)
                countExpr = countExpr.extract(m_ptr_bits - 1, 0);
              sizeBytesExpr = countExpr * m_solver.getValue().ctx().bv_val(
                                              elemBytes, m_ptr_bits);
              known = countExpr.is_numeral();
            }
          }
          ensureObject(ai,
                       ("alloca." + F->getName().str() + "." +
                        std::to_string((uintptr_t)ai)),
                       sizeBytesExpr, known);
        }
      }
    }

    // Get a path tree.
    for (auto &bb : F->getBasicBlockList()) {
      for (const auto &pred : predecessors(&bb)) {
        if (m_backedges[&bb].contains(pred) || &bb == pred)
          continue;

        m_bbpaths[pred].push_back(&bb);
      }
    }

    m_solver.getValue().push();
    pushSymFrame();
    m_smt_mem->push();
    pushConstraintFrame();

    // add function arg constraints (integers + pointers).
    for (auto &arg : F->args()) {
      const std::string arg_name =
          (F->getName() + "." + std::to_string(arg.getArgNo())).str();
      if (arg.getType()->isIntegerTy()) {
        const auto argv = m_solver.getValue().ctx().bv_const(
            arg_name.c_str(), arg.getType()->getIntegerBitWidth());
        setSym(&arg, argv);
        m_bug_detection->add_range_cons(
            m_range_analysis->get_range_by_bb(
                &arg, &(F->getEntryBlock()), m_func2range_info, m_global2range),
            argv, m_solver.getValue(),
            [this](const z3::expr &e) { addConstraint(e); });
      } else if (arg.getType()->isPointerTy()) {
        const auto argv = m_solver.getValue().ctx().bv_const(
            (arg_name + ".ptr").c_str(), m_ptr_bits);
        setSym(&arg, argv);
      }
    }

    path_solving(&(F->getEntryBlock()), nullptr);

    m_smt_mem->pop();
    popSymFrame();
    popConstraintFrame();
    m_solver.getValue().pop();

    // Report analysis time
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - m_function_start_time)
                       .count();
    MKINT_LOG() << "Completed analysis of function " << F->getName() << " in "
                << elapsed << " seconds";
  }
}

void MKintPass::path_solving(BasicBlock *cur, BasicBlock *pred) {
  // Backedge check must come before the path counter so that loop back-edges
  // do not consume path budget.
  if (m_backedges[cur].contains(pred))
    return;

  // Cap path exploration to avoid blowups on large CFGs.
  if (m_path_limit > 0) {
    if (m_paths_explored++ >= m_path_limit) {
      if (!m_path_limit_hit) {
        MKINT_WARN() << "Path exploration limit reached for function "
                     << (cur && cur->getParent() ? cur->getParent()->getName()
                                                 : "<unknown>")
                     << " (limit=" << m_path_limit << "). Analysis incomplete.";
        m_path_limit_hit = true;
      }
      return;
    }
  }

  // Check for timeout
  if (m_function_timeout > 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - m_function_start_time)
                       .count();
    if (elapsed > static_cast<int64_t>(m_function_timeout)) {
      MKINT_WARN() << "Timeout reached for function "
                   << cur->getParent()->getName() << " after " << elapsed
                   << " seconds. Analysis incomplete.";
      return;
    }
  }

  // Track this basic block in the current execution path
  std::string bbDesc = "Basic block ";
  if (cur->hasName()) {
    bbDesc += cur->getName().str();
  } else {
    bbDesc += "<unnamed>";
  }
  if (cur->getParent()) {
    bbDesc += " in function " + cur->getParent()->getName().str();
  }

  PathPoint pathPoint(cur, nullptr, bbDesc);
  m_bug_detection->addPathPoint(pathPoint);

  if (nullptr != pred) {
    auto *terminator = pred->getTerminator();
    auto *br = dyn_cast<BranchInst>(terminator);
    if (br) {
      if (br->isConditional()) {
        const bool is_true_br = br->getSuccessor(0) == cur;
        Value *cond = br->getCondition();

        // If the condition is an ICmp, encode it precisely (including pointer
        // equality).
        z3::expr condBool = m_solver.getValue().ctx().bool_val(true);
        if (auto *cmp = dyn_cast<ICmpInst>(cond)) {
          // Do not hard-prune based on range analysis; let SMT decide
          // satisfiability.

          auto *lhs = cmp->getOperand(0);
          auto *rhs = cmp->getOperand(1);

          if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
            const auto l = getIntExpr(lhs, pred, nullptr);
            const auto r = getIntExpr(rhs, pred, nullptr);
            switch (cmp->getPredicate()) {
            case ICmpInst::ICMP_EQ:
              condBool = (l == r);
              break;
            case ICmpInst::ICMP_NE:
              condBool = (l != r);
              break;
            case ICmpInst::ICMP_SGT:
              condBool = z3::sgt(l, r);
              break;
            case ICmpInst::ICMP_SGE:
              condBool = z3::sge(l, r);
              break;
            case ICmpInst::ICMP_SLT:
              condBool = z3::slt(l, r);
              break;
            case ICmpInst::ICMP_SLE:
              condBool = z3::sle(l, r);
              break;
            case ICmpInst::ICMP_UGT:
              condBool = z3::ugt(l, r);
              break;
            case ICmpInst::ICMP_UGE:
              condBool = z3::uge(l, r);
              break;
            case ICmpInst::ICMP_ULT:
              condBool = z3::ult(l, r);
              break;
            case ICmpInst::ICMP_ULE:
              condBool = z3::ule(l, r);
              break;
            default:
              MKINT_WARN() << "Unsupported icmp predicate in branch condition: "
                           << *cmp;
              condBool = m_solver.getValue().ctx().bool_val(true);
              break;
            }
          } else if (lhs->getType()->isPointerTy() &&
                     rhs->getType()->isPointerTy()) {
            const auto l = getPtrExpr(lhs, pred, nullptr);
            const auto r = getPtrExpr(rhs, pred, nullptr);
            switch (cmp->getPredicate()) {
            case ICmpInst::ICMP_EQ:
              condBool = (l == r);
              break;
            case ICmpInst::ICMP_NE:
              condBool = (l != r);
              break;
            default:
              MKINT_WARN()
                  << "Unsupported pointer icmp predicate in branch condition: "
                  << *cmp;
              condBool = m_solver.getValue().ctx().bool_val(true);
              break;
            }
          } else {
            MKINT_WARN()
                << "Unsupported icmp operand types in branch condition: "
                << *cmp;
          }

          // Record the branch decision in the bug path.
          std::string branchDesc = std::string("Taking ") +
                                   (is_true_br ? "true" : "false") +
                                   " branch from condition: ";
          llvm::raw_string_ostream brOS(branchDesc);
          brOS << *cmp;
          PathPoint branchPoint(pred, cmp, brOS.str());
          m_bug_detection->addPathPoint(branchPoint);
        } else if (cond->getType()->isIntegerTy(1)) {
          // Generic i1 condition.
          const auto c = getIntExpr(cond, pred, nullptr);
          condBool = (c == m_solver.getValue().ctx().bv_val(1, 1));
        } else {
          MKINT_WARN() << "Unsupported branch condition: " << *cond;
        }

        addConstraint(is_true_br ? condBool : !condBool);
        if (m_solver.getValue().check() == z3::unsat) {
          MKINT_DEBUG() << "[SMT Solving] Pruned unsat edge into "
                        << cur->getName();
          return;
        }
      }
    } else if (auto *swt = dyn_cast<SwitchInst>(terminator)) {
      auto *cond = swt->getCondition();
      if (cond->getType()->isIntegerTy()) {
        if (swt->getDefaultDest() == cur) { // default
          // not (all)
          for (auto c : swt->cases()) {
            auto *case_val = c.getCaseValue();
            addConstraint(getIntExpr(cond, pred, nullptr) !=
                          bvValFromAPInt(m_solver.getValue().ctx(),
                                         case_val->getValue()));
          }
        } else {
          for (auto c : swt->cases()) {
            if (c.getCaseSuccessor() == cur) {
              auto *case_val = c.getCaseValue();
              addConstraint(getIntExpr(cond, pred, nullptr) ==
                            bvValFromAPInt(m_solver.getValue().ctx(),
                                           case_val->getValue()));
              break;
            }
          }
        }
      }
    } else if (isa<InvokeInst>(terminator) || isa<IndirectBrInst>(terminator) ||
               isa<CallBrInst>(terminator)) {
      // No additional constraints; successor feasibility handled
      // conservatively.
    } else {
      // try catch... (thank god, C does not have try-catch)
      // indirectbr... ?
      MKINT_WARN() << "Unknown terminator; proceeding conservatively: "
                   << *pred->getTerminator();
    }
  }

  // Resolve PHI nodes in the current block based on the predecessor edge.
  if (pred) {
    for (auto &inst : cur->getInstList()) {
      auto *phi = dyn_cast<PHINode>(&inst);
      if (!phi)
        break;
      if (Value *incoming = phi->getIncomingValueForBlock(pred)) {
        const auto incomingExpr = getValueExpr(incoming, pred, nullptr);
        setSym(phi, incomingExpr);
        if (phi->getType()->isIntegerTy()) {
          m_bug_detection->add_range_cons(
              m_range_analysis->get_range_by_bb(phi, cur, m_func2range_info,
                                                m_global2range),
              incomingExpr, m_solver.getValue(),
              [this](const z3::expr &e) { addConstraint(e); });
        }
      }
    }
  }

  for (auto &inst : cur->getInstList()) {
    if (isa<PHINode>(&inst))
      continue;

    if (auto *assumeI = dyn_cast<AssumeInst>(&inst)) {
      // llvm.assume(cond) adds a constraint to the current path.
      const auto *cond = assumeI->getArgOperand(0);
      if (cond && cond->getType()->isIntegerTy(1)) {
        const auto c = getIntExpr(cond, cur, pred);
        addConstraint(c == m_solver.getValue().ctx().bv_val(1, 1));
        if (m_solver.getValue().check() == z3::unsat)
          return;
      }
      continue;
    }

    if (auto *woi = dyn_cast<WithOverflowInst>(&inst)) {
      // Encode arithmetic/overflow semantics for llvm.*with.overflow
      // intrinsics. Bug checking: if overflow is satisfiable under current path
      // constraints, report an overflow.
      if (CheckIntOverflow) {
        z3::expr res = m_solver.getValue().ctx().bv_val(
            0, woi->getArgOperand(0)->getType()->getIntegerBitWidth());
        z3::expr ov = m_solver.getValue().ctx().bool_val(false);
        if (computeWithOverflow(
                woi, m_solver.getValue(),
                [&](const llvm::Value *x) { return getIntExpr(x, cur, pred); },
                res, ov)) {
          if (checkBugCondition(woi, interr::INT_OVERFLOW, ov)) {
            m_overflow_insts.insert(woi);
            if (m_bug_detection)
              m_bug_detection->recordBug(woi, interr::INT_OVERFLOW);
          }
        }
      }
      continue;
    }

    // Model memory intrinsics.
    // These often appear as `llvm.memset/memcpy/memmove.*` and bypass normal
    // CallInst handling.
    if (auto *memsetI = dyn_cast<MemSetInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memsetI->getRawDest(), cur, pred);
      const auto val = getIntExpr(memsetI->getValue(), cur, pred);
      if (const auto len = getConstantU64(memsetI->getLength())) {
        if (*len <= kMaxBytes) {
          const Value *obj = getObjectForPtr(memsetI->getRawDest());
          if (obj && m_obj_mem.count(obj)) {
            const auto base = m_obj_base[obj].getValue();
            const auto off = dst - base;
            z3::expr b = val;
            const unsigned bw = b.get_sort().bv_size();
            if (bw < 8)
              b = z3::zext(b, 8 - bw);
            else if (bw > 8)
              b = b.extract(7, 0);
            z3::expr curMem = m_obj_mem[obj].getValue();
            for (uint64_t i = 0; i < *len; ++i) {
              curMem = z3::store(
                  curMem, off + m_solver.getValue().ctx().bv_val(i, m_ptr_bits),
                  b);
            }
            m_obj_mem[obj] = curMem;
          } else {
            m_smt_mem->memsetBytes(dst, val, *len);
          }
        } else {
          const Value *obj = getObjectForPtr(memsetI->getRawDest());
          if (obj)
            havocObject(obj, "memset_large");
          else
            m_smt_mem->havoc("memset_large");
        }
        if (!maybeCheckOOB(memsetI, memsetI->getRawDest(), *len, cur, pred))
          return;
      } else {
        const Value *obj = getObjectForPtr(memsetI->getRawDest());
        if (obj)
          havocObject(obj, "memset_sym");
        else
          m_smt_mem->havoc("memset_sym");
      }
      continue;
    }

    if (auto *memcpyI = dyn_cast<MemCpyInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memcpyI->getRawDest(), cur, pred);
      const auto src = getPtrExpr(memcpyI->getRawSource(), cur, pred);
      if (const auto len = getConstantU64(memcpyI->getLength())) {
        if (*len <= kMaxBytes) {
          const Value *dstObj = getObjectForPtr(memcpyI->getRawDest());
          const Value *srcObj = getObjectForPtr(memcpyI->getRawSource());
          if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
              m_obj_mem.count(srcObj)) {
            const auto dstBase = m_obj_base[dstObj].getValue();
            const auto srcBase = m_obj_base[srcObj].getValue();
            z3::expr dstOff = dst - dstBase;
            z3::expr srcOff = src - srcBase;
            z3::expr dstMem = m_obj_mem[dstObj].getValue();
            z3::expr srcMem = m_obj_mem[srcObj].getValue();
            for (uint64_t i = 0; i < *len; ++i) {
              z3::expr b = z3::select(
                  srcMem,
                  srcOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits));
              dstMem = z3::store(
                  dstMem,
                  dstOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits), b);
            }
            m_obj_mem[dstObj] = dstMem;
          } else if (dstObj) {
            havocObject(dstObj, "memcpy_unknown_src");
          } else {
            m_smt_mem->memcpyBytes(dst, src, *len);
          }
        } else {
          const Value *obj = getObjectForPtr(memcpyI->getRawDest());
          if (obj)
            havocObject(obj, "memcpy_large");
          else
            m_smt_mem->havoc("memcpy_large");
        }
        if (!maybeCheckOOB(memcpyI, memcpyI->getRawDest(), *len, cur, pred))
          return;
      } else {
        const Value *obj = getObjectForPtr(memcpyI->getRawDest());
        if (obj)
          havocObject(obj, "memcpy_sym");
        else
          m_smt_mem->havoc("memcpy_sym");
      }
      continue;
    }

    if (auto *memmoveI = dyn_cast<MemMoveInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memmoveI->getRawDest(), cur, pred);
      const auto src = getPtrExpr(memmoveI->getRawSource(), cur, pred);
      if (const auto len = getConstantU64(memmoveI->getLength())) {
        if (*len <= kMaxBytes) {
          // Our memory is a functional array; a forward copy is sufficient for
          // modeling memmove.
          const Value *dstObj = getObjectForPtr(memmoveI->getRawDest());
          const Value *srcObj = getObjectForPtr(memmoveI->getRawSource());
          if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
              m_obj_mem.count(srcObj)) {
            const auto dstBase = m_obj_base[dstObj].getValue();
            const auto srcBase = m_obj_base[srcObj].getValue();
            z3::expr dstOff = dst - dstBase;
            z3::expr srcOff = src - srcBase;
            z3::expr dstMem = m_obj_mem[dstObj].getValue();
            z3::expr srcMem = m_obj_mem[srcObj].getValue();
            for (uint64_t i = 0; i < *len; ++i) {
              z3::expr b = z3::select(
                  srcMem,
                  srcOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits));
              dstMem = z3::store(
                  dstMem,
                  dstOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits), b);
            }
            m_obj_mem[dstObj] = dstMem;
          } else if (dstObj) {
            havocObject(dstObj, "memmove_unknown_src");
          } else {
            m_smt_mem->memcpyBytes(dst, src, *len);
          }
        } else {
          const Value *obj = getObjectForPtr(memmoveI->getRawDest());
          if (obj)
            havocObject(obj, "memmove_large");
          else
            m_smt_mem->havoc("memmove_large");
        }
        if (!maybeCheckOOB(memmoveI, memmoveI->getRawDest(), *len, cur, pred))
          return;
      } else {
        const Value *obj = getObjectForPtr(memmoveI->getRawDest());
        if (obj)
          havocObject(obj, "memmove_sym");
        else
          m_smt_mem->havoc("memmove_sym");
      }
      continue;
    }

    if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
      // Bind the alloca instruction to its base address.
      if (m_obj_base.count(ai)) {
        setSym(ai, m_obj_base[ai].getValue());
      } else {
        const uint64_t elemBytes =
            m_dl->getTypeAllocSize(ai->getAllocatedType());
        z3::expr sizeBytesExpr =
            m_solver.getValue().ctx().bv_val(elemBytes, m_ptr_bits);
        bool known = true;
        if (ai->isArrayAllocation()) {
          if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
            sizeBytesExpr = m_solver.getValue().ctx().bv_val(
                elemBytes * ci->getZExtValue(), m_ptr_bits);
          } else {
            auto countExpr = getIntExpr(ai->getArraySize(), cur, pred);
            const unsigned cbw = countExpr.get_sort().bv_size();
            if (cbw < m_ptr_bits)
              countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
            else if (cbw > m_ptr_bits)
              countExpr = countExpr.extract(m_ptr_bits - 1, 0);
            sizeBytesExpr = countExpr * m_solver.getValue().ctx().bv_val(
                                            elemBytes, m_ptr_bits);
            known = countExpr.is_numeral();
          }
        }
        ensureObject(ai,
                     ("alloca." + cur->getParent()->getName().str() + "." +
                      std::to_string((uintptr_t)ai)),
                     sizeBytesExpr, known);
        setSym(ai, m_obj_base[ai].getValue());
      }
      continue;
    }

    if (auto *gep = dyn_cast<GetElementPtrInst>(&inst)) {
      setSym(gep, getPtrExpr(gep, cur, pred));
      continue;
    }

    if (auto *load = dyn_cast<LoadInst>(&inst)) {
      const unsigned bytes =
          static_cast<unsigned>(m_dl->getTypeStoreSize(load->getType()));
      if (!maybeCheckOOB(load, load->getPointerOperand(), bytes, cur, pred))
        return;
      if (load->getType()->isIntegerTy()) {
        const auto addr = getPtrExpr(load->getPointerOperand(), cur, pred);
        const unsigned bw = load->getType()->getIntegerBitWidth();
        const Value *obj = getObjectForPtr(load->getPointerOperand());
        z3::expr v = m_smt_mem->loadInt(addr, bw, bytes, isLittleEndian());
        if (obj && m_obj_mem.count(obj)) {
          const auto base = m_obj_base[obj].getValue();
          const auto off = addr - base;
          z3::expr bytesExpr = loadBytesFromMem(m_obj_mem[obj].getValue(), off,
                                                bytes, isLittleEndian());
          const unsigned loadedBits = bytes * 8;
          if (bw == loadedBits) {
            v = bytesExpr;
          } else if (bw < loadedBits) {
            v = bytesExpr.extract(bw - 1, 0);
          } else {
            v = z3::zext(bytesExpr, bw - loadedBits);
          }
        }
        setSym(load, v);
        if (m_robust_universal_unknown_loads) {
          bool unknown = true;
          const Value *underlying =
              llvm::getUnderlyingObject(load->getPointerOperand());
          if (underlying && m_obj_base.count(underlying)) {
            unknown = false;
          }
          if (unknown) {
            registerUniversal(v);
          }
        }
        if (m_robust_universal_external_globals) {
          if (const auto *gv = dyn_cast_or_null<GlobalVariable>(
                  llvm::getUnderlyingObject(load->getPointerOperand()))) {
            if (gv->isDeclaration()) {
              registerUniversal(v);
            }
          }
        }
        if (!m_bug_detection->add_range_cons(
                m_range_analysis->get_range_by_bb(load, cur, m_func2range_info,
                                                  m_global2range),
                v, m_solver.getValue(),
                [this](const z3::expr &e) { addConstraint(e); }))
          return;
      }
      continue;
    }

    if (auto *store = dyn_cast<StoreInst>(&inst)) {
      auto *val = store->getValueOperand();
      const unsigned bytes =
          static_cast<unsigned>(m_dl->getTypeStoreSize(val->getType()));
      if (!maybeCheckOOB(store, store->getPointerOperand(), bytes, cur, pred))
        return;
      if (val && val->getType()->isIntegerTy()) {
        const auto addr = getPtrExpr(store->getPointerOperand(), cur, pred);
        const unsigned bw = val->getType()->getIntegerBitWidth();
        const auto v = getIntExpr(val, cur, pred);
        const Value *obj = getObjectForPtr(store->getPointerOperand());
        if (obj && m_obj_mem.count(obj)) {
          const auto base = m_obj_base[obj].getValue();
          const auto off = addr - base;
          z3::expr newMem = storeBytesToMem(m_obj_mem[obj].getValue(), off, v,
                                            bytes, isLittleEndian());
          m_obj_mem[obj] = newMem;
        } else {
          m_smt_mem->storeInt(addr, v, bw, bytes, isLittleEndian());
        }
      }
      continue;
    }

    if (auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
      const Value *obj = getObjectForPtr(rmw->getPointerOperand());
      if (obj)
        havocObject(obj, "atomicrmw");
      else
        m_smt_mem->havoc("atomicrmw");
      if (rmw->getType()->isIntegerTy()) {
        (void)getIntExpr(rmw, cur, pred);
      }
      continue;
    }

    if (auto *cx = dyn_cast<AtomicCmpXchgInst>(&inst)) {
      const Value *obj = getObjectForPtr(cx->getPointerOperand());
      if (obj)
        havocObject(obj, "cmpxchg");
      else
        m_smt_mem->havoc("cmpxchg");
      continue;
    }

    if (auto *call = dyn_cast<CallInst>(&inst)) {
      // Model common libc memory routines (when they survive as regular calls).
      if (Function *callee = call->getCalledFunction()) {
        constexpr uint64_t kMaxBytes = 256;
        const auto name = callee->getName();

        if ((name == "memset" || name == "__memset") && call->arg_size() >= 3) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto val = getIntExpr(call->getArgOperand(1), cur, pred);
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj && m_obj_mem.count(obj)) {
                const auto base = m_obj_base[obj].getValue();
                const auto off = dst - base;
                z3::expr b = val;
                const unsigned bw = b.get_sort().bv_size();
                if (bw < 8)
                  b = z3::zext(b, 8 - bw);
                else if (bw > 8)
                  b = b.extract(7, 0);
                z3::expr curMem = m_obj_mem[obj].getValue();
                for (uint64_t i = 0; i < *len; ++i) {
                  curMem = z3::store(
                      curMem,
                      off + m_solver.getValue().ctx().bv_val(i, m_ptr_bits), b);
                }
                m_obj_mem[obj] = curMem;
              } else {
                m_smt_mem->memsetBytes(dst, val, *len);
              }
            } else {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj)
                havocObject(obj, "memset_large");
              else
                m_smt_mem->havoc("memset_large");
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            const Value *obj = getObjectForPtr(call->getArgOperand(0));
            if (obj)
              havocObject(obj, "memset_sym");
            else
              m_smt_mem->havoc("memset_sym");
          }
          // memset returns dst.
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if ((name == "memcpy" || name == "__memcpy") && call->arg_size() >= 3) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto src = getPtrExpr(call->getArgOperand(1), cur, pred);
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              const Value *dstObj = getObjectForPtr(call->getArgOperand(0));
              const Value *srcObj = getObjectForPtr(call->getArgOperand(1));
              if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
                  m_obj_mem.count(srcObj)) {
                const auto dstBase = m_obj_base[dstObj].getValue();
                const auto srcBase = m_obj_base[srcObj].getValue();
                z3::expr dstOff = dst - dstBase;
                z3::expr srcOff = src - srcBase;
                z3::expr dstMem = m_obj_mem[dstObj].getValue();
                z3::expr srcMem = m_obj_mem[srcObj].getValue();
                for (uint64_t i = 0; i < *len; ++i) {
                  z3::expr b = z3::select(
                      srcMem,
                      srcOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits));
                  dstMem = z3::store(
                      dstMem,
                      dstOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits),
                      b);
                }
                m_obj_mem[dstObj] = dstMem;
              } else if (dstObj) {
                havocObject(dstObj, "memcpy_unknown_src");
              } else {
                m_smt_mem->memcpyBytes(dst, src, *len);
              }
            } else {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj)
                havocObject(obj, "memcpy_large");
              else
                m_smt_mem->havoc("memcpy_large");
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            const Value *obj = getObjectForPtr(call->getArgOperand(0));
            if (obj)
              havocObject(obj, "memcpy_sym");
            else
              m_smt_mem->havoc("memcpy_sym");
          }
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if ((name == "memmove" || name == "__memmove") &&
            call->arg_size() >= 3) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto src = getPtrExpr(call->getArgOperand(1), cur, pred);
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              const Value *dstObj = getObjectForPtr(call->getArgOperand(0));
              const Value *srcObj = getObjectForPtr(call->getArgOperand(1));
              if (dstObj && srcObj && m_obj_mem.count(dstObj) &&
                  m_obj_mem.count(srcObj)) {
                const auto dstBase = m_obj_base[dstObj].getValue();
                const auto srcBase = m_obj_base[srcObj].getValue();
                z3::expr dstOff = dst - dstBase;
                z3::expr srcOff = src - srcBase;
                z3::expr dstMem = m_obj_mem[dstObj].getValue();
                z3::expr srcMem = m_obj_mem[srcObj].getValue();
                for (uint64_t i = 0; i < *len; ++i) {
                  z3::expr b = z3::select(
                      srcMem,
                      srcOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits));
                  dstMem = z3::store(
                      dstMem,
                      dstOff + m_solver.getValue().ctx().bv_val(i, m_ptr_bits),
                      b);
                }
                m_obj_mem[dstObj] = dstMem;
              } else if (dstObj) {
                havocObject(dstObj, "memmove_unknown_src");
              } else {
                m_smt_mem->memcpyBytes(dst, src, *len);
              }
            } else {
              const Value *obj = getObjectForPtr(call->getArgOperand(0));
              if (obj)
                havocObject(obj, "memmove_large");
              else
                m_smt_mem->havoc("memmove_large");
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            const Value *obj = getObjectForPtr(call->getArgOperand(0));
            if (obj)
              havocObject(obj, "memmove_sym");
            else
              m_smt_mem->havoc("memmove_sym");
          }
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        // Unknown call with memory side effects: conservatively havoc memory so
        // subsequent loads don't assume stale/zero contents.
        const bool is_allocator =
            (name == "malloc" || name == "calloc" || name == "realloc" ||
             name == "free" || name == "kmalloc" || name == "kzalloc" ||
             name == "vmalloc");
        if (!is_allocator && !call->doesNotAccessMemory() &&
            !call->onlyReadsMemory()) {
          bool any = false;
          for (const auto *obj : m_obj_list) {
            if (callMayModObject(call, obj)) {
              havocObject(obj, "call_sidefx");
              any = true;
            }
          }
          if (!any) {
            m_smt_mem->havoc("call_sidefx");
          }
        }
      }

      // Model common allocators as fresh, disjoint heap objects.
      if (call->getType()->isPointerTy()) {
        Function *callee = call->getCalledFunction();
        if (callee) {
          const auto name = callee->getName();
          z3::expr sizeBytes = m_solver.getValue().ctx().bv_val(0, m_ptr_bits);
          bool sizeKnown = false;
          if (name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc") {
            if (call->arg_size() >= 1 &&
                call->getArgOperand(0)->getType()->isIntegerTy()) {
              sizeBytes = getIntExpr(call->getArgOperand(0), cur, pred);
              const unsigned abw = sizeBytes.get_sort().bv_size();
              if (abw < m_ptr_bits)
                sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
              if (abw > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
            }
            sizeKnown = sizeBytes.is_numeral();
          } else if (name == "calloc") {
            if (call->arg_size() >= 2 &&
                call->getArgOperand(0)->getType()->isIntegerTy() &&
                call->getArgOperand(1)->getType()->isIntegerTy()) {
              auto n = getIntExpr(call->getArgOperand(0), cur, pred);
              auto m = getIntExpr(call->getArgOperand(1), cur, pred);
              const unsigned n_bw = n.get_sort().bv_size();
              const unsigned m_bw = m.get_sort().bv_size();
              const unsigned target =
                  std::max(std::max(n_bw, m_bw), m_ptr_bits);
              if (n_bw < target)
                n = z3::zext(n, target - n_bw);
              if (m_bw < target)
                m = z3::zext(m, target - m_bw);
              sizeBytes = (n * m);
              if (target > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
            }
            sizeKnown = sizeBytes.is_numeral();
          } else if (name == "realloc") {
            if (call->arg_size() >= 2 &&
                call->getArgOperand(1)->getType()->isIntegerTy()) {
              sizeBytes = getIntExpr(call->getArgOperand(1), cur, pred);
              const unsigned abw = sizeBytes.get_sort().bv_size();
              if (abw < m_ptr_bits)
                sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
              if (abw > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
            }
            sizeKnown = sizeBytes.is_numeral();
          }

          if (name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc" || name == "calloc" || name == "realloc") {
            ensureObject(call,
                         ("heap." + cur->getParent()->getName().str() + "." +
                          std::to_string((uintptr_t)call)),
                         sizeBytes, sizeKnown);
            setSym(call, m_obj_base[call].getValue());
            continue;
          }
        }

        // Unknown pointer-returning call: treat as fresh pointer value.
        setSym(call, getPtrExpr(call, cur, pred));
        continue;
      }

      // Inline asm return value can be treated as unknown.
      if (call->getType()->isIntegerTy() && call->isInlineAsm()) {
        auto v = getIntExpr(call, cur, pred);
        if (m_robust_universal_inline_asm) {
          registerUniversal(v);
        }
        continue;
      }
    }

    // Integer SSA: keep existing bug checks, but also allow values derived from
    // loads, selects, etc.
    if (inst.getType()->isIntegerTy()) {
      if (auto *op = dyn_cast<BinaryOperator>(&inst)) {
        (void)getIntExpr(op->getOperand(0), cur, pred);
        (void)getIntExpr(op->getOperand(1), cur, pred);
        m_bug_detection->binary_check(
            op, m_solver.getValue(), m_v2sym, m_overflow_insts,
            m_bad_shift_insts, m_div_zero_insts, m_robust_reachability,
            &m_path_constraints, &m_universal_vars,
            [this, op](interr type, const z3::expr &q) {
              dumpEfConstraint(op, type, q);
            },
            [this](interr type) { return isRobustBugEnabled(type); });
        if (!addWellDefinedConstraints(op, cur, pred))
          return;
        const auto r = m_bug_detection->binary_op_propagate(
            op, m_v2sym, m_solver.getValue());
        setSym(op, r);
        if (!m_bug_detection->add_range_cons(
                m_range_analysis->get_range_by_bb(op, cur, m_func2range_info,
                                                  m_global2range),
                r, m_solver.getValue(),
                [this](const z3::expr &e) { addConstraint(e); }))
          return;
      } else if (auto *op = dyn_cast<CastInst>(&inst)) {
        (void)getValueExpr(op->getOperand(0), cur, pred);
        const auto r = m_bug_detection->cast_op_propagate(op, m_v2sym,
                                                          m_solver.getValue());
        setSym(op, r);
        if (!m_bug_detection->add_range_cons(
                m_range_analysis->get_range_by_bb(op, cur, m_func2range_info,
                                                  m_global2range),
                r, m_solver.getValue(),
                [this](const z3::expr &e) { addConstraint(e); }))
          return;
      } else {
        (void)getIntExpr(&inst, cur, pred);
      }
    } else if (inst.getType()->isPointerTy()) {
      (void)getPtrExpr(&inst, cur, pred);
    }
  }

  for (auto *succ : m_bbpaths[cur]) {
    m_solver.getValue().push();
    pushSymFrame();
    m_smt_mem->push();
    pushConstraintFrame();
    // Record the path depth before recursing so we can restore it on return.
    const size_t pathDepthBefore = m_bug_detection->getCurrentPath().size();
    path_solving(succ, cur);
    // Restore the path to the depth it had before the recursive call.
    // This is symmetric with the addPathPoint calls inside path_solving.
    auto currentPath = m_bug_detection->getCurrentPath();
    while (currentPath.size() > pathDepthBefore)
      currentPath.pop_back();
    m_bug_detection->setCurrentPath(currentPath);
    m_smt_mem->pop();
    popSymFrame();
    popConstraintFrame();
    m_solver.getValue().pop();
  }
}

void MKintPass::generateSarifReport(const std::string &filename) {
  if (m_bug_detection) {
    m_bug_detection->generateSarifReport(filename, m_impossible_branches,
                                         m_gep_oob, m_overflow_insts,
                                         m_bad_shift_insts, m_div_zero_insts);
  }
}

void MKintPass::pushSymFrame() {
  m_sym_change_frames.push_back(m_sym_change_log.size());
}

void MKintPass::popSymFrame() {
  if (m_sym_change_frames.empty())
    return;
  const size_t frameStart = m_sym_change_frames.back();
  m_sym_change_frames.pop_back();

  while (m_sym_change_log.size() > frameStart) {
    const SymChange ch = m_sym_change_log.back();
    m_sym_change_log.pop_back();
    if (!ch.key)
      continue;
    if (ch.hadOld) {
      m_v2sym[ch.key] = ch.oldValue;
    } else {
      m_v2sym.erase(ch.key);
    }
  }
}

void MKintPass::setSym(const Value *v, const z3::expr &e) {
  SymChange ch;
  ch.key = v;
  auto it = m_v2sym.find(v);
  ch.hadOld = (it != m_v2sym.end());
  if (ch.hadOld)
    ch.oldValue = it->second;
  m_sym_change_log.push_back(ch);
  m_v2sym[v] = e;
}

void MKintPass::pushConstraintFrame() {
  m_constraint_frames.push_back(m_path_constraints.size());
}

void MKintPass::popConstraintFrame() {
  if (m_constraint_frames.empty())
    return;
  const size_t frameStart = m_constraint_frames.back();
  m_constraint_frames.pop_back();
  while (m_path_constraints.size() > frameStart) {
    m_path_constraints.pop_back();
  }
}

void MKintPass::addConstraint(const z3::expr &e) {
  m_solver.getValue().add(e);
  if (m_robust_reachability) {
    m_path_constraints.push_back(e);
  }
}

const Value *MKintPass::getObjectForPtr(const Value *ptr) const {
  if (!ptr)
    return nullptr;
  const Value *stripped = ptr->stripPointerCasts();
  const Value *obj = llvm::getUnderlyingObject(stripped);
  if (obj && m_obj_base.count(obj))
    return obj;
  return nullptr;
}

z3::expr MKintPass::loadBytesFromMem(const z3::expr &mem,
                                     const z3::expr &offset, unsigned numBytes,
                                     bool littleEndian) const {
  auto &ctx = m_solver.getValue().ctx();
  if (numBytes == 0)
    return ctx.bv_val(0, 0);
  z3::expr result = z3::select(
      mem, offset + ctx.bv_val(littleEndian ? (numBytes - 1) : 0, m_ptr_bits));
  for (unsigned i = 1; i < numBytes; ++i) {
    const unsigned byteIndex = littleEndian ? (numBytes - 1 - i) : i;
    z3::expr b = z3::select(mem, offset + ctx.bv_val(byteIndex, m_ptr_bits));
    result = z3::concat(result, b);
  }
  return result;
}

z3::expr MKintPass::storeBytesToMem(const z3::expr &mem, const z3::expr &offset,
                                    const z3::expr &value, unsigned numBytes,
                                    bool littleEndian) const {
  auto &ctx = m_solver.getValue().ctx();
  if (numBytes == 0)
    return mem;
  const unsigned targetBits = numBytes * 8;
  z3::expr v = value;
  const unsigned vbw = v.get_sort().bv_size();
  if (vbw < targetBits)
    v = z3::zext(v, targetBits - vbw);
  else if (vbw > targetBits)
    v = v.extract(targetBits - 1, 0);

  z3::expr cur = mem;
  for (unsigned i = 0; i < numBytes; ++i) {
    const unsigned valueByteIndex = littleEndian ? i : (numBytes - 1 - i);
    const unsigned lo = valueByteIndex * 8;
    const unsigned hi = lo + 7;
    z3::expr byteVal = v.extract(hi, lo);
    cur = z3::store(cur, offset + ctx.bv_val(i, m_ptr_bits), byteVal);
  }
  return cur;
}

void MKintPass::havocObject(const Value *obj, const std::string &hint) {
  if (!obj)
    return;
  auto &ctx = m_solver.getValue().ctx();
  const auto id = g_obj_mem_id.fetch_add(1, std::memory_order_relaxed);
  const std::string name = "%objmem." + hint + "." + std::to_string(id);
  m_obj_mem[obj] = ctx.constant(
      name.c_str(), ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
}

bool MKintPass::callMayModObject(llvm::CallBase *call, const Value *obj) const {
  if (!call || !obj || !m_aa)
    return true;
  llvm::LocationSize size = llvm::LocationSize::afterPointer();
  if (m_obj_size.count(obj)) {
    if (auto sizeExpr = m_obj_size.find(obj)->second) {
      if (sizeExpr->is_numeral()) {
        uint64_t bytes = 0;
        if (Z3_get_numeral_uint64(sizeExpr->ctx(), *sizeExpr, &bytes)) {
          size = llvm::LocationSize::precise(bytes);
        }
      }
    }
  }
  llvm::MemoryLocation loc(obj, size);
  auto modref = m_aa->getModRefInfo(call, loc);
  return llvm::isModSet(modref);
}

z3::expr MKintPass::buildPathConstraintConjunction() const {
  auto &ctx = m_solver.getValue().ctx();
  if (m_path_constraints.empty())
    return ctx.bool_val(true);
  z3::expr_vector pcs(ctx);
  for (const auto &c : m_path_constraints)
    pcs.push_back(c);
  return z3::mk_and(pcs);
}

void MKintPass::registerUniversal(const z3::expr &e) {
  if (!m_robust_reachability)
    return;
  Z3_ast key = e;
  if (m_universal_var_ids.insert(key).second) {
    m_universal_vars.push_back(e);
  }
}

void MKintPass::dumpEfConstraint(const Instruction *inst, interr type,
                                 const z3::expr &q) const {
  if (m_dump_ef_path.empty())
    return;
  std::ofstream out(m_dump_ef_path, std::ios::app);
  if (!out.is_open())
    return;
  out << "=== EF Constraint ===\n";
  if (inst && inst->getParent() && inst->getParent()->getParent()) {
    out << "Function: " << inst->getParent()->getParent()->getName().str()
        << "\n";
  }
  if (type != interr::NONE) {
    out << "Bug: " << bugTypeToString(type) << "\n";
  }
  if (inst) {
    std::string instStr;
    llvm::raw_string_ostream instOS(instStr);
    instOS << *inst;
    out << "Inst: " << instOS.str() << "\n";
  }
  out << q.to_string() << "\n";
  out << "=====================\n";
}

bool MKintPass::checkBugCondition(const Instruction *inst, interr type,
                                  const z3::expr &bugCond) {
  if (!m_robust_reachability) {
    m_solver.getValue().push();
    m_solver.getValue().add(bugCond);
    const bool sat = (m_solver.getValue().check() == z3::sat);
    m_solver.getValue().pop();
    return sat;
  }
  if (type != interr::NONE && !isRobustBugEnabled(type)) {
    return false;
  }

  auto &ctx = m_solver.getValue().ctx();
  z3::solver qsolver(ctx);
  z3::expr body = buildPathConstraintConjunction() && bugCond;
  z3::expr q = body;
  if (!m_universal_vars.empty()) {
    z3::expr_vector uvars(ctx);
    for (const auto &v : m_universal_vars)
      uvars.push_back(v);
    q = z3::forall(uvars, body);
    qsolver.add(q);
  } else {
    qsolver.add(body);
  }
  dumpEfConstraint(inst, type, q);
  return qsolver.check() == z3::sat;
}

bool MKintPass::isRobustBugEnabled(interr type) const {
  if (m_robust_bug_filter.empty())
    return true;
  return m_robust_bug_filter.count(type) > 0;
}

void MKintPass::parseRobustBugFilter(const std::string &csv) {
  m_robust_bug_filter.clear();
  if (csv.empty())
    return;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t end = csv.find(',', start);
    if (end == std::string::npos)
      end = csv.size();
    auto token = csv.substr(start, end - start);
    auto trim = [](const std::string &s) {
      size_t b = s.find_first_not_of(" \t");
      size_t e = s.find_last_not_of(" \t");
      if (b == std::string::npos)
        return std::string();
      return s.substr(b, e - b + 1);
    };
    token = trim(token);
    if (token == "overflow") {
      m_robust_bug_filter.insert(interr::INT_OVERFLOW);
    } else if (token == "div0" || token == "div") {
      m_robust_bug_filter.insert(interr::DIV_BY_ZERO);
    } else if (token == "shift") {
      m_robust_bug_filter.insert(interr::BAD_SHIFT);
    } else if (token == "oob" || token == "array-oob") {
      m_robust_bug_filter.insert(interr::ARRAY_OOB);
    } else if (token == "dead") {
      m_robust_bug_filter.insert(interr::DEAD_TRUE_BR);
      m_robust_bug_filter.insert(interr::DEAD_FALSE_BR);
    }
    start = end + 1;
  }
}

bool MKintPass::isLittleEndian() const {
  return m_dl ? m_dl->isLittleEndian() : true;
}

void MKintPass::ensureObject(const Value *obj, const std::string &hintName,
                             const z3::expr &sizeBytes, bool sizeKnown) {
  if (m_obj_base.count(obj))
    return;

  auto &ctx = m_solver.getValue().ctx();
  const auto base = ctx.bv_const(hintName.c_str(), m_ptr_bits);

  m_obj_base[obj] = base;
  m_obj_size[obj] = sizeBytes;
  m_obj_list.push_back(obj);
  havocObject(obj, hintName);

  // Basic well-formedness: keep base non-zero to avoid conflating with null.
  addConstraint(base != ctx.bv_val(0, m_ptr_bits));

  // Disjointness constraints against previously created objects.
  for (const auto *other : m_obj_list) {
    if (other == obj)
      continue;
    if (!m_obj_base.count(other) || !m_obj_size.count(other))
      continue;
    const auto otherBase = m_obj_base[other].getValue();
    const auto otherSize = m_obj_size[other].getValue();
    if (sizeKnown) {
      // Non-overlap: [base, base+size) does not overlap [otherBase,
      // otherBase+otherSize)
      const auto endThis = base + sizeBytes;
      const auto endOther = otherBase + otherSize;
      addConstraint(z3::ule(endThis, otherBase) || z3::ule(endOther, base));
    } else {
      // Unknown size: at least force distinct bases.
      addConstraint(base != otherBase);
    }
  }

  // Avoid modular wraparound when computing [base, base+size) for known-size
  // objects.
  if (sizeKnown) {
    addConstraint(z3::bvadd_no_overflow(base, sizeBytes, /*is_signed=*/false));
  }
}

bool MKintPass::maybeCheckOOB(const Instruction *at, const Value *ptrOperand,
                              uint64_t accessBytes, BasicBlock *cur,
                              BasicBlock *pred) {
  if (!CheckArrayOOB)
    return true;
  if (!at || !ptrOperand || !m_solver || !m_smt_mem)
    return true;
  if (accessBytes == 0)
    return true;

  const Value *stripped = ptrOperand->stripPointerCasts();
  const auto *gep = dyn_cast<GetElementPtrInst>(stripped);
  if (!gep)
    return true; // keep reporting consistent with existing ARRAY_OOB pipeline

  const Value *obj = llvm::getUnderlyingObject(stripped);
  if (!obj)
    return true;
  if (!m_obj_base.count(obj) || !m_obj_size.count(obj))
    return true;

  const auto baseOpt = m_obj_base[obj];
  const auto sizeOpt = m_obj_size[obj];
  if (!baseOpt.hasValue() || !sizeOpt.hasValue())
    return true;

  auto &solver = m_solver.getValue();
  auto &ctx = solver.ctx();
  const auto &base = baseOpt.getValue();
  const auto &size = sizeOpt.getValue();
  const auto addr = getPtrExpr(ptrOperand, cur, pred);
  const auto len = ctx.bv_val(accessBytes, m_ptr_bits);

  // In-bounds check for a byte range: addr >= base && addr + len <= base +
  // size, without wrapping.
  const z3::expr noWrap = z3::bvadd_no_overflow(addr, len, /*is_signed=*/false);
  const z3::expr inBounds =
      (z3::uge(addr, base) && z3::ule(addr + len, base + size) && noWrap);

  if (checkBugCondition(gep, interr::ARRAY_OOB, !inBounds)) {
    m_gep_oob.insert(const_cast<GetElementPtrInst *>(gep));
    if (m_bug_detection) {
      m_bug_detection->recordBug(gep, interr::ARRAY_OOB);
    }
  }

  // Constrain the remaining exploration to defined, in-bounds behaviors (LLVM
  // semantics for out-of-bounds are UB).
  addConstraint(inBounds);
  return solver.check() != z3::unsat;
}

bool MKintPass::addWellDefinedConstraints(BinaryOperator *op, BasicBlock *cur,
                                          BasicBlock *pred) {
  if (!op || !m_solver)
    return true;
  auto &solver = m_solver.getValue();
  auto &ctx = solver.ctx();

  const auto lhs = getIntExpr(op->getOperand(0), cur, pred);
  const auto rhs = getIntExpr(op->getOperand(1), cur, pred);
  const unsigned bw = lhs.get_sort().bv_size();

  bool added = false;
  const auto addAndMark = [&](const z3::expr &e) {
    addConstraint(e);
    added = true;
  };

  switch (op->getOpcode()) {
  case Instruction::UDiv:
  case Instruction::URem:
  case Instruction::SDiv:
  case Instruction::SRem:
    // Div/rem by zero is poison; keep exploring only defined paths.
    addAndMark(rhs != ctx.bv_val(0, bw));
    if (op->getOpcode() == Instruction::SDiv) {
      // Signed division overflow (INT_MIN / -1) is poison in LLVM.
      addAndMark(z3::bvsdiv_no_overflow(lhs, rhs));
    }
    break;
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    // Shift amount must be < bitwidth; otherwise poison.
    addAndMark(z3::ult(rhs, ctx.bv_val(bw, bw)));
    break;
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
    if (auto *ofop = dyn_cast<OverflowingBinaryOperator>(op)) {
      const bool nsw = ofop->hasNoSignedWrap();
      const bool nuw = ofop->hasNoUnsignedWrap();
      if (nuw) {
        if (op->getOpcode() == Instruction::Add) {
          addAndMark(z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/false));
        } else if (op->getOpcode() == Instruction::Sub) {
          addAndMark(z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/false));
        } else if (op->getOpcode() == Instruction::Mul) {
          addAndMark(z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/false));
        }
      }
      if (nsw) {
        if (op->getOpcode() == Instruction::Add) {
          addAndMark(z3::bvadd_no_overflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvadd_no_underflow(lhs, rhs));
        } else if (op->getOpcode() == Instruction::Sub) {
          addAndMark(z3::bvsub_no_underflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvsub_no_overflow(lhs, rhs));
        } else if (op->getOpcode() == Instruction::Mul) {
          addAndMark(z3::bvmul_no_overflow(lhs, rhs, /*is_signed=*/true));
          addAndMark(z3::bvmul_no_underflow(lhs, rhs));
        }
      }
    }
    break;
  default:
    break;
  }

  if (!added)
    return true;
  return solver.check() != z3::unsat;
}

z3::expr MKintPass::getValueExpr(const Value *v, BasicBlock *cur,
                                 BasicBlock *pred) {
  if (!v)
    return m_solver.getValue().ctx().bv_val(0, 1);
  if (v->getType()->isIntegerTy())
    return getIntExpr(v, cur, pred);
  if (v->getType()->isPointerTy())
    return getPtrExpr(v, cur, pred);
  // Unsupported sort: return a fresh 1-bit value to keep the solver going.
  const std::string name = "%unsupported." + std::to_string((uintptr_t)v);
  return m_solver.getValue().ctx().bv_const(name.c_str(), 1);
}

z3::expr MKintPass::getIntExpr(const Value *v, BasicBlock *cur,
                               BasicBlock *pred) {
  auto it = m_v2sym.find(v);
  if (it != m_v2sym.end())
    return it->second.getValue();

  auto &ctx = m_solver.getValue().ctx();

  if (const auto *ci = dyn_cast<ConstantInt>(v)) {
    return bvValFromAPInt(ctx, ci->getValue());
  }

  if (const auto *fr = dyn_cast<FreezeInst>(v)) {
    auto r = getIntExpr(fr->getOperand(0), cur, pred);
    setSym(v, r);
    return r;
  }

  if (const auto *ev = dyn_cast<ExtractValueInst>(v)) {
    if (ev->getNumIndices() == 1) {
      const unsigned idx = *ev->idx_begin();
      if (const auto *woi =
              dyn_cast<WithOverflowInst>(ev->getAggregateOperand())) {
        z3::expr res = ctx.bv_val(
            0, woi->getArgOperand(0)->getType()->getIntegerBitWidth());
        z3::expr ov = ctx.bool_val(false);
        if (computeWithOverflow(
                woi, m_solver.getValue(),
                [&](const llvm::Value *x) { return getIntExpr(x, cur, pred); },
                res, ov)) {
          if (idx == 0) {
            setSym(v, res);
            return res;
          }
          if (idx == 1) {
            auto b = boolToBv1(ov);
            setSym(v, b);
            return b;
          }
        }
      }
    }
  }

  if (const auto *pti = dyn_cast<PtrToIntInst>(v)) {
    auto p = getPtrExpr(pti->getOperand(0), cur, pred);
    const unsigned bw = pti->getType()->getIntegerBitWidth();
    if (bw < m_ptr_bits)
      p = p.extract(bw - 1, 0);
    else if (bw > m_ptr_bits)
      p = z3::zext(p, bw - m_ptr_bits);
    setSym(v, p);
    return p;
  }

  if (const auto *itp = dyn_cast<IntToPtrInst>(v)) {
    auto i = getIntExpr(itp->getOperand(0), cur, pred);
    const unsigned ibw = i.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      i = z3::zext(i, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      i = i.extract(m_ptr_bits - 1, 0);
    // IntToPtr result is a pointer, not int; fall back to fresh int symbol.
  }

  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    if (sel->getType()->isIntegerTy()) {
      auto c = getIntExpr(sel->getCondition(), cur, pred);
      auto t = getIntExpr(sel->getTrueValue(), cur, pred);
      auto f = getIntExpr(sel->getFalseValue(), cur, pred);
      z3::expr condBool = (c == ctx.bv_val(1, 1));
      z3::expr r = z3::ite(condBool, t, f);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *icmp = dyn_cast<ICmpInst>(v)) {
    auto *lhs = icmp->getOperand(0);
    auto *rhs = icmp->getOperand(1);
    z3::expr condBool = ctx.bool_val(true);
    if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
      const auto l = getIntExpr(lhs, cur, pred);
      const auto r = getIntExpr(rhs, cur, pred);
      switch (icmp->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        condBool = (l == r);
        break;
      case ICmpInst::ICMP_NE:
        condBool = (l != r);
        break;
      case ICmpInst::ICMP_SGT:
        condBool = z3::sgt(l, r);
        break;
      case ICmpInst::ICMP_SGE:
        condBool = z3::sge(l, r);
        break;
      case ICmpInst::ICMP_SLT:
        condBool = z3::slt(l, r);
        break;
      case ICmpInst::ICMP_SLE:
        condBool = z3::sle(l, r);
        break;
      case ICmpInst::ICMP_UGT:
        condBool = z3::ugt(l, r);
        break;
      case ICmpInst::ICMP_UGE:
        condBool = z3::uge(l, r);
        break;
      case ICmpInst::ICMP_ULT:
        condBool = z3::ult(l, r);
        break;
      case ICmpInst::ICMP_ULE:
        condBool = z3::ule(l, r);
        break;
      default:
        break;
      }
    } else if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
      const auto l = getPtrExpr(lhs, cur, pred);
      const auto r = getPtrExpr(rhs, cur, pred);
      switch (icmp->getPredicate()) {
      case ICmpInst::ICMP_EQ:
        condBool = (l == r);
        break;
      case ICmpInst::ICMP_NE:
        condBool = (l != r);
        break;
      default:
        break;
      }
    }
    auto bv = z3::ite(condBool, ctx.bv_val(1, 1), ctx.bv_val(0, 1));
    setSym(v, bv);
    return bv;
  }

  if (const auto *phi = dyn_cast<PHINode>(v)) {
    // Ideally resolved on block entry. If not, keep it symbolic.
    const std::string name = "%phi." + std::to_string((uintptr_t)phi);
    auto r = ctx.bv_const(name.c_str(), phi->getType()->getIntegerBitWidth());
    setSym(v, r);
    return r;
  }

  if (const auto *call = dyn_cast<CallInst>(v)) {
    if (call->getType()->isIntegerTy()) {
      const std::string name = "%call." + std::to_string((uintptr_t)call);
      auto r =
          ctx.bv_const(name.c_str(), call->getType()->getIntegerBitWidth());
      setSym(v, r);
      bool unknown_call = true;
      if (auto *callee = call->getCalledFunction()) {
        if (!callee->isDeclaration()) {
          unknown_call = false;
        }
      }
      if (unknown_call) {
        if (m_robust_reachability) {
          registerUniversal(r);
        }
      }
      if (cur) {
        m_bug_detection->add_range_cons(
            m_range_analysis->get_range_by_bb(call, cur, m_func2range_info,
                                              m_global2range),
            r, m_solver.getValue(),
            [this](const z3::expr &e) { addConstraint(e); });
      }
      return r;
    }
  }

  // Default: fresh int with range constraints if available.
  const unsigned bw = v->getType()->getIntegerBitWidth();
  const std::string name = "%int." + std::to_string((uintptr_t)v);
  auto r = ctx.bv_const(name.c_str(), bw);
  setSym(v, r);
  if (cur) {
    m_bug_detection->add_range_cons(
        m_range_analysis->get_range_by_bb(v, cur, m_func2range_info,
                                          m_global2range),
        r, m_solver.getValue(),
        [this](const z3::expr &e) { addConstraint(e); });
  }
  return r;
}

z3::expr MKintPass::gepOffsetBytes(const GetElementPtrInst *gep,
                                   BasicBlock *cur, BasicBlock *pred) {
  auto &ctx = m_solver.getValue().ctx();
  if (!gep || !m_dl)
    return ctx.bv_val(0, m_ptr_bits);

  // Fast path: all-constant GEP.
  APInt constOff(m_ptr_bits, 0);
  if (gep->accumulateConstantOffset(*m_dl, constOff)) {
    return ctx.bv_val(constOff.getZExtValue(), m_ptr_bits);
  }

  z3::expr off = ctx.bv_val(0, m_ptr_bits);
  Type *ty = gep->getSourceElementType();
  unsigned idxNo = 0;
  for (const auto *idxIt = gep->idx_begin(); idxIt != gep->idx_end();
       ++idxIt, ++idxNo) {
    Value *idxV = idxIt->get();
    if (!idxV)
      continue;

    if (auto *st = dyn_cast<StructType>(ty)) {
      auto *ci = dyn_cast<ConstantInt>(idxV);
      if (!ci) {
        // Non-constant struct indices are not supported in LLVM IR, but be
        // defensive.
        const std::string name =
            "%gep.structidx." + std::to_string((uintptr_t)gep);
        return ctx.bv_const(name.c_str(), m_ptr_bits);
      }
      const unsigned field = static_cast<unsigned>(ci->getZExtValue());
      const auto *layout = m_dl->getStructLayout(st);
      off = off + ctx.bv_val(layout->getElementOffset(field), m_ptr_bits);
      ty = st->getElementType(field);
      continue;
    }

    uint64_t elemBytes = 0;
    if (auto *at = dyn_cast<ArrayType>(ty)) {
      elemBytes = m_dl->getTypeAllocSize(at->getElementType());
      ty = at->getElementType();
    } else {
      // First index on a scalar pointer: step by the source element size.
      elemBytes = m_dl->getTypeAllocSize(ty);
    }

    z3::expr idx = getIntExpr(idxV, cur, pred);
    const unsigned ibw = idx.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      idx = z3::sext(idx, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      idx = idx.extract(m_ptr_bits - 1, 0);
    off = off + (idx * ctx.bv_val(elemBytes, m_ptr_bits));
  }

  return off;
}

z3::expr MKintPass::getPtrExpr(const Value *v, BasicBlock *cur,
                               BasicBlock *pred) {
  auto it = m_v2sym.find(v);
  if (it != m_v2sym.end())
    return it->second.getValue();

  auto &ctx = m_solver.getValue().ctx();

  if (isa<ConstantPointerNull>(v)) {
    return ctx.bv_val(0, m_ptr_bits);
  }

  if (const auto *gv = dyn_cast<GlobalVariable>(v)) {
    if (!m_obj_base.count(gv)) {
      const uint64_t bytes = m_dl->getTypeAllocSize(gv->getValueType());
      ensureObject(gv, ("global." + gv->getName()).str(),
                   ctx.bv_val(bytes, m_ptr_bits), true);
    }
    setSym(v, m_obj_base[gv].getValue());
    return m_obj_base[gv].getValue();
  }

  if (const auto *fr = dyn_cast<FreezeInst>(v)) {
    auto r = getPtrExpr(fr->getOperand(0), cur, pred);
    setSym(v, r);
    return r;
  }

  if (const auto *ai = dyn_cast<AllocaInst>(v)) {
    if (!m_obj_base.count(ai)) {
      const uint64_t elemBytes = m_dl->getTypeAllocSize(ai->getAllocatedType());
      z3::expr sizeBytesExpr = ctx.bv_val(elemBytes, m_ptr_bits);
      bool known = true;
      if (ai->isArrayAllocation()) {
        if (auto *ci = dyn_cast<ConstantInt>(ai->getArraySize())) {
          sizeBytesExpr =
              ctx.bv_val(elemBytes * ci->getZExtValue(), m_ptr_bits);
        } else {
          auto countExpr = getIntExpr(ai->getArraySize(), cur, pred);
          const unsigned cbw = countExpr.get_sort().bv_size();
          if (cbw < m_ptr_bits)
            countExpr = z3::zext(countExpr, m_ptr_bits - cbw);
          else if (cbw > m_ptr_bits)
            countExpr = countExpr.extract(m_ptr_bits - 1, 0);
          sizeBytesExpr = countExpr * ctx.bv_val(elemBytes, m_ptr_bits);
          known = countExpr.is_numeral();
        }
      }
      ensureObject(ai,
                   ("alloca." + ai->getFunction()->getName().str() + "." +
                    std::to_string((uintptr_t)ai)),
                   sizeBytesExpr, known);
    }
    setSym(v, m_obj_base[ai].getValue());
    return m_obj_base[ai].getValue();
  }

  if (const auto *arg = dyn_cast<Argument>(v)) {
    if (arg->getType()->isPointerTy()) {
      const std::string name = (arg->getParent()->getName() + ".argptr" +
                                std::to_string(arg->getArgNo()))
                                   .str();
      auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *gep = dyn_cast<GetElementPtrInst>(v)) {
    auto base = getPtrExpr(gep->getPointerOperand(), cur, pred);
    auto off = gepOffsetBytes(gep, cur, pred);
    auto r = base + off;
    setSym(v, r);
    return r;
  }

  if (const auto *bc = dyn_cast<BitCastInst>(v)) {
    auto r = getPtrExpr(bc->getOperand(0), cur, pred);
    setSym(v, r);
    return r;
  }

  if (const auto *itp = dyn_cast<IntToPtrInst>(v)) {
    auto i = getIntExpr(itp->getOperand(0), cur, pred);
    const unsigned ibw = i.get_sort().bv_size();
    if (ibw < m_ptr_bits)
      i = z3::zext(i, m_ptr_bits - ibw);
    else if (ibw > m_ptr_bits)
      i = i.extract(m_ptr_bits - 1, 0);
    setSym(v, i);
    return i;
  }

  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    if (sel->getType()->isPointerTy()) {
      auto c = getIntExpr(sel->getCondition(), cur, pred);
      auto t = getPtrExpr(sel->getTrueValue(), cur, pred);
      auto f = getPtrExpr(sel->getFalseValue(), cur, pred);
      z3::expr condBool = (c == ctx.bv_val(1, 1));
      auto r = z3::ite(condBool, t, f);
      setSym(v, r);
      return r;
    }
  }

  if (const auto *phi = dyn_cast<PHINode>(v)) {
    const std::string name = "%phi.ptr." + std::to_string((uintptr_t)phi);
    auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
    setSym(v, r);
    return r;
  }

  if (const auto *call = dyn_cast<CallInst>(v)) {
    if (call->getType()->isPointerTy()) {
      const std::string name = "%call.ptr." + std::to_string((uintptr_t)call);
      auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
      setSym(v, r);
      return r;
    }
  }

  const std::string name = "%ptr." + std::to_string((uintptr_t)v);
  auto r = ctx.bv_const(name.c_str(), m_ptr_bits);
  setSym(v, r);
  return r;
}

} // namespace kint

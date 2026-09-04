#include "Checker/KINT/BugDetection.h"

#include "Checker/Framework/SARIF.h"
#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"
#include "Utils/Types/range.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Operator.h>
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

template <interr err, typename StrRet = const char *> constexpr StrRet mkstr() {
  if (err == interr::NONE) {
    return "none";
  } else if (err == interr::INT_OVERFLOW) {
    return "integer overflow";
  } else if (err == interr::DIV_BY_ZERO) {
    return "divide by zero";
  } else if (err == interr::BAD_SHIFT) {
    return "bad shift";
  } else if (err == interr::ARRAY_OOB) {
    return "array index out of bound";
  } else if (err == interr::DEAD_TRUE_BR) {
    return "impossible true branch";
  } else if (err == interr::DEAD_FALSE_BR) {
    return "impossible false branch";
  } else {
    static_assert(err == interr::NONE || err == interr::INT_OVERFLOW ||
                      err == interr::DIV_BY_ZERO || err == interr::BAD_SHIFT ||
                      err == interr::ARRAY_OOB || err == interr::DEAD_TRUE_BR ||
                      err == interr::DEAD_FALSE_BR,
                  "unknown error type");
    return "";
  }
}

inline const char *mkstr(interr err) {
  switch (err) {
  case interr::NONE:
    return mkstr<interr::NONE>();
  case interr::INT_OVERFLOW:
    return mkstr<interr::INT_OVERFLOW>();
  case interr::DIV_BY_ZERO:
    return mkstr<interr::DIV_BY_ZERO>();
  case interr::BAD_SHIFT:
    return mkstr<interr::BAD_SHIFT>();
  case interr::ARRAY_OOB:
    return mkstr<interr::ARRAY_OOB>();
  case interr::DEAD_TRUE_BR:
    return mkstr<interr::DEAD_TRUE_BR>();
  case interr::DEAD_FALSE_BR:
    return mkstr<interr::DEAD_FALSE_BR>();
  default:
    MKINT_CHECK_ABORT(false) << "unknown error type" << static_cast<int>(err);
  }
  return "";
}

constexpr const char *MKINT_IR_ERR = "mkint.err";

template <interr err_t, typename I>
typename std::enable_if<std::is_pointer<I>::value>::type
BugDetection::mark_err(I inst) {
  auto &ctx = inst->getContext();
  std::string prefix = "";
  if (MDNode *omd = inst->getMetadata(MKINT_IR_ERR)) {
    prefix = cast<MDString>(omd->getOperand(0))->getString().str() + " + ";
  }
  auto md = MDNode::get(ctx, MDString::get(ctx, prefix + mkstr<err_t>()));
  inst->setMetadata(MKINT_IR_ERR, md);
}

template <interr err_t, typename I>
typename std::enable_if<!std::is_pointer<I>::value>::type
BugDetection::mark_err(I &inst) {
  mark_err<err_t>(&inst);
}

void BugDetection::binary_check(
    BinaryOperator *op, z3::solver &solver,
    const DenseMap<const Value *, std::optional<z3::expr>> &v2sym,
    std::set<Instruction *> &overflow_insts,
    std::set<Instruction *> &bad_shift_insts,
    std::set<Instruction *> &div_zero_insts, bool robust_mode,
    const std::vector<z3::expr> *path_constraints,
    const std::vector<z3::expr> *universal_vars,
    const std::function<void(interr, const z3::expr &)> &dump,
    const std::function<bool(interr)> &robustFilter) {
  // Skip checks if all checkers are disabled
  if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift)
    return;

  const auto &lhs_bv = this->v2sym(op->getOperand(0), v2sym, solver);
  const auto &rhs_bv = this->v2sym(op->getOperand(1), v2sym, solver);
  const auto rhs_bits = rhs_bv.get_sort().bv_size();

  // Always check both signed and unsigned overflow regardless of nsw/nuw flags
  // These flags only promise no overflow in well-defined code, but we're
  // detecting potential bugs
  const bool preferSigned = false;
  const bool preferUnsigned = false;

  const auto check =
      [&](interr et, bool is_signed, const z3::expr &bugCond) {
        if (robust_mode && robustFilter && !robustFilter(et)) {
          return;
        }
        bool sat = false;
        std::unique_ptr<z3::model> model;
        if (!robust_mode) {
          solver.push();
          solver.add(bugCond);
          sat = (solver.check() == z3::sat);
          if (sat) {
            model = std::make_unique<z3::model>(solver.get_model());
          }
          solver.pop();
        } else {
          auto &ctx = solver.ctx();
          z3::solver qsolver(ctx);
          z3::expr pc = ctx.bool_val(true);
          if (path_constraints && !path_constraints->empty()) {
            z3::expr_vector pcs(ctx);
            for (const auto &c : *path_constraints)
              pcs.push_back(c);
            pc = z3::mk_and(pcs);
          }
          z3::expr body = pc && bugCond;
          z3::expr q = body;
          if (universal_vars && !universal_vars->empty()) {
            z3::expr_vector uvars(ctx);
            for (const auto &v : *universal_vars)
              uvars.push_back(v);
            q = z3::forall(uvars, body);
            qsolver.add(q);
          } else {
            qsolver.add(body);
          }
          sat = (qsolver.check() == z3::sat);
          if (dump) {
            dump(et, q);
          }
        }

        if (sat) {

          if (!robust_mode) {
            // Evaluate the model captured under the bug condition.
            try {
              if (model) {
                auto lhs_bin = model->eval(lhs_bv, true);
                auto rhs_bin = model->eval(rhs_bv, true);
                // TODO: we can also evaluate the propagated expression to get the result under the counterexample, which can be helpful for debugging
                MKINT_WARN() << "Counterexample for " << mkstr(et) << " ("
                             << (is_signed ? "signed" : "unsigned") << "): "
                             << "lhs = " << lhs_bin << ", rhs = " << rhs_bin;
              }
            } catch (const z3::exception &e) {
              MKINT_WARN() << "Could not retrieve counter example: " << e.msg();
            }
          }

          // Record bug with its path
          this->recordBugWithPath(op, et);

          switch (et) {
          case interr::INT_OVERFLOW:
            overflow_insts.insert(op);
            break;
          case interr::BAD_SHIFT:
            bad_shift_insts.insert(op);
            break;
          case interr::DIV_BY_ZERO:
            div_zero_insts.insert(op);
            break;
          default:
            break;
          }
        }
      };
  switch (op->getOpcode()) {
  case Instruction::Add:
    if (!CheckIntOverflow)
      break;

    if (!preferSigned) {
      check(interr::INT_OVERFLOW, false,
            !z3::bvadd_no_overflow(lhs_bv, rhs_bv, false));
    }
    if (!preferUnsigned) {
      check(interr::INT_OVERFLOW, true,
            (!z3::bvadd_no_overflow(lhs_bv, rhs_bv, true) ||
             !z3::bvadd_no_underflow(lhs_bv, rhs_bv)));
    }
    break;

  case Instruction::Sub:
    if (!CheckIntOverflow)
      break;

    if (!preferSigned) {
      check(interr::INT_OVERFLOW, false,
            !z3::bvsub_no_underflow(lhs_bv, rhs_bv, false));
    }
    if (!preferUnsigned) {
      check(interr::INT_OVERFLOW, true,
            (!z3::bvsub_no_underflow(lhs_bv, rhs_bv, true) ||
             !z3::bvsub_no_overflow(lhs_bv, rhs_bv)));
    }
    break;

  case Instruction::Mul:
    if (!CheckIntOverflow)
      break;

    if (!preferSigned) {
      check(interr::INT_OVERFLOW, false,
            !z3::bvmul_no_overflow(lhs_bv, rhs_bv, false));
    }
    if (!preferUnsigned) {
      check(interr::INT_OVERFLOW, true,
            (!z3::bvmul_no_overflow(lhs_bv, rhs_bv, true) ||
             !z3::bvmul_no_underflow(lhs_bv, rhs_bv)));
    }
    break;

  case Instruction::URem:
  case Instruction::UDiv:
    if (!CheckDivByZero)
      break;

    check(interr::DIV_BY_ZERO, false,
          rhs_bv == solver.ctx().bv_val(0, rhs_bits));
    break;

  case Instruction::SRem:
  case Instruction::SDiv: // can be overflow or divisor == 0
    if (CheckDivByZero) {
      check(interr::DIV_BY_ZERO, true,
            rhs_bv == solver.ctx().bv_val(0, rhs_bits));
    }

    if (CheckIntOverflow) {
      check(interr::INT_OVERFLOW, true,
            !z3::bvsdiv_no_overflow(lhs_bv, rhs_bv));
    }
    break;

  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
    if (!CheckBadShift)
      break;

    check(interr::BAD_SHIFT, false,
          rhs_bv >= solver.ctx().bv_val(rhs_bits, rhs_bits));
    break;

  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  default:
    break;
  }
}

z3::expr BugDetection::binary_op_propagate(
    BinaryOperator *op,
    const DenseMap<const Value *, std::optional<z3::expr>> &v2sym,
    z3::solver &solver) {
  auto lhs = this->v2sym(op->getOperand(0), v2sym, solver);
  auto rhs = this->v2sym(op->getOperand(1), v2sym, solver);
  switch (op->getOpcode()) {
  case Instruction::Add:
    return lhs + rhs;
  case Instruction::Sub:
    return lhs - rhs;
  case Instruction::Mul:
    return lhs * rhs;
  case Instruction::URem:
    return z3::urem(lhs, rhs);
  case Instruction::UDiv:
    return z3::udiv(lhs, rhs);
  case Instruction::SRem:
    return z3::srem(lhs, rhs);
  case Instruction::SDiv: // can be overflow or divisor == 0
    return lhs / rhs;
  case Instruction::Shl:
    return z3::shl(lhs, rhs);
  case Instruction::LShr:
    return z3::lshr(lhs, rhs);
  case Instruction::AShr:
    return z3::ashr(lhs, rhs);
  case Instruction::And:
    return lhs & rhs;
  case Instruction::Or:
    return lhs | rhs;
  case Instruction::Xor:
    return lhs ^ rhs;
  default:
    break;
  }

  MKINT_CHECK_ABORT(false) << "unsupported binary op: " << *op;
  return lhs; // dummy
}

z3::expr BugDetection::v2sym(
    const Value *v,
    const DenseMap<const Value *, std::optional<z3::expr>> &v2sym_map,
    z3::solver &solver) {
  auto it = v2sym_map.find(v);
  if (it != v2sym_map.end())
    return it->second.value();

  auto *lconst = dyn_cast<ConstantInt>(v);
  if (lconst != nullptr) {
    return bvValFromAPInt(solver.ctx(), lconst->getValue());
  }

  // Fallback: create a stable fresh symbol instead of aborting.
  // Use a deterministic name so repeated calls map to the same Z3 symbol.
  unsigned bw = 1;
  if (v && v->getType()->isIntegerTy()) {
    bw = v->getType()->getIntegerBitWidth();
  }
  const std::string name =
      "%sym.missing." + std::to_string(reinterpret_cast<uintptr_t>(v));
  MKINT_WARN() << "unsupported value -> symbol mapping; using fresh symbol: "
               << name;
  return solver.ctx().bv_const(name.c_str(), bw);
}

void BugDetection::recordBugWithPath(const Instruction *inst, interr type) {
  if (!inst)
    return;

  // Create a new BugPath for this bug
  BugPath bugPath(inst, type);
  bugPath.path = m_current_path;

  // Store it in the map
  m_bug_paths[BugKey(inst, type)] = bugPath;
}

void BugDetection::recordBug(const Instruction *inst, interr type) {
  recordBugWithPath(inst, type);
}

z3::expr BugDetection::cast_op_propagate(
    CastInst *op,
    const DenseMap<const Value *, std::optional<z3::expr>> &v2sym,
    z3::solver &solver) {
  const uint32_t bits = op->getType()->getIntegerBitWidth();
  const std::string fallback_sym = "%cast." + std::to_string(op->getValueID());

  // Guard: source operand must be an integer type for Trunc/ZExt/SExt.
  // Non-integer sources (e.g. FPToSI, PtrToInt, BitCast) are handled by
  // returning a fresh unconstrained symbol rather than crashing.
  auto *srcOp = op->getOperand(0);
  if (!srcOp || !srcOp->getType()->isIntegerTy()) {
    MKINT_WARN() << "cast_op_propagate: non-integer source for "
                 << op->getOpcodeName() << "; using fresh symbol.";
    return solver.ctx().bv_const(fallback_sym.c_str(), bits);
  }

  const auto src = this->v2sym(srcOp, v2sym, solver);
  const uint32_t src_bits = srcOp->getType()->getIntegerBitWidth();

  switch (op->getOpcode()) {
  case CastInst::Trunc:
    if (bits >= src_bits) {
      // Defensive: should not happen in well-formed IR, but avoid UB in
      // extract.
      MKINT_WARN() << "Trunc to wider/equal type; using fresh symbol.";
      return solver.ctx().bv_const(fallback_sym.c_str(), bits);
    }
    return src.extract(bits - 1, 0);
  case CastInst::ZExt:
    if (bits <= src_bits) {
      MKINT_WARN() << "ZExt to narrower/equal type; using fresh symbol.";
      return solver.ctx().bv_const(fallback_sym.c_str(), bits);
    }
    return z3::zext(src, bits - src_bits);
  case CastInst::SExt:
    if (bits <= src_bits) {
      MKINT_WARN() << "SExt to narrower/equal type; using fresh symbol.";
      return solver.ctx().bv_const(fallback_sym.c_str(), bits);
    }
    return z3::sext(src, bits - src_bits);
  default:
    MKINT_WARN() << "Unhandled Cast Instruction " << op->getOpcodeName()
                 << ". Using fresh symbol.";
  }

  return solver.ctx().bv_const(fallback_sym.c_str(), bits);
}

void BugDetection::mark_errors(
    const std::map<ICmpInst *, bool> &impossible_branches,
    const std::set<GetElementPtrInst *> &gep_oob,
    const std::set<Instruction *> &overflow_insts,
    const std::set<Instruction *> &bad_shift_insts,
    const std::set<Instruction *> &div_zero_insts) {
  if (CheckDeadBranch) {
    for (auto &cmp_istbr_pair : impossible_branches) {
      auto *cmp = cmp_istbr_pair.first;
      auto is_tbr = cmp_istbr_pair.second;
      if (is_tbr)
        mark_err<interr::DEAD_TRUE_BR>(cmp);
      else
        mark_err<interr::DEAD_FALSE_BR>(cmp);
    }
  }

  if (CheckArrayOOB) {
    for (auto *gep : gep_oob) {
      mark_err<interr::ARRAY_OOB>(gep);
    }
  }

  if (CheckIntOverflow) {
    for (auto *inst : overflow_insts) {
      mark_err<interr::INT_OVERFLOW>(inst);
    }
  }

  if (CheckBadShift) {
    for (auto *inst : bad_shift_insts) {
      mark_err<interr::BAD_SHIFT>(inst);
    }
  }

  if (CheckDivByZero) {
    for (auto *inst : div_zero_insts) {
      mark_err<interr::DIV_BY_ZERO>(inst);
    }
  }
}

void BugDetection::generateSarifReport(
    const std::string &filename,
    const std::map<ICmpInst *, bool> &impossible_branches,
    const std::set<GetElementPtrInst *> &gep_oob,
    const std::set<Instruction *> &overflow_insts,
    const std::set<Instruction *> &bad_shift_insts,
    const std::set<Instruction *> &div_zero_insts) {
  sarif::SarifLog sarifLog("Kint", "1.0.0");

  // Define rules for each bug type
  sarifLog.addRule(sarif::Rule("INT_OVERFLOW", "Integer Overflow",
                               "Integer arithmetic operation may overflow"));
  sarifLog.addRule(
      sarif::Rule("DIV_BY_ZERO", "Division by Zero",
                  "Division or modulo operation may have zero divisor"));
  sarifLog.addRule(
      sarif::Rule("BAD_SHIFT", "Bad Shift",
                  "Shift operation may have shift amount >= bit width"));
  sarifLog.addRule(sarif::Rule("ARRAY_OOB", "Array Out of Bounds",
                               "Array index may be out of bounds"));
  sarifLog.addRule(sarif::Rule("DEAD_TRUE_BR", "Impossible True Branch",
                               "Branch condition can never be true"));
  sarifLog.addRule(sarif::Rule("DEAD_FALSE_BR", "Impossible False Branch",
                               "Branch condition can never be false"));

  // Helper lambda to add a result for an instruction
  auto addBugResult = [&sarifLog, this](const Instruction *inst,
                                        interr bugType) {
    if (!inst)
      return;

    sarif::Result result(
        [bugType]() {
          switch (bugType) {
          case interr::NONE:
            return "NONE";
          case interr::INT_OVERFLOW:
            return "INT_OVERFLOW";
          case interr::DIV_BY_ZERO:
            return "DIV_BY_ZERO";
          case interr::BAD_SHIFT:
            return "BAD_SHIFT";
          case interr::ARRAY_OOB:
            return "ARRAY_OOB";
          case interr::DEAD_TRUE_BR:
            return "DEAD_TRUE_BR";
          case interr::DEAD_FALSE_BR:
            return "DEAD_FALSE_BR";
          default:
            return "UNKNOWN";
          }
        }(),
        mkstr(bugType));

    result.level = sarif::Level::Error;

    // Get location from debug info
    sarif::Location loc = sarif::utils::createLocationFromInstruction(inst);
    if (!loc.file.empty() && loc.line > 0) {
      // Add instruction text as snippet
      std::string instStr;
      llvm::raw_string_ostream instOS(instStr);
      instOS << *inst;
      loc.snippet = instOS.str();

      result.locations.push_back(loc);
    }

    // Add execution path as code flow if available
    auto pathIt = m_bug_paths.find(BugKey(inst, bugType));
    if (pathIt != m_bug_paths.end() && !pathIt->second.path.empty()) {
      sarif::CodeFlow codeFlow;
      codeFlow.message =
          "Execution path leading to " + std::string(mkstr(bugType));

      int order = 1;
      for (const auto &pathPoint : pathIt->second.path) {
        // Create location for this path point
        sarif::Location pathLoc;

        // Try to get location from the instruction if available
        const Instruction *pathInst =
            pathPoint.inst ? pathPoint.inst
                           : (pathPoint.bb ? &pathPoint.bb->front() : nullptr);

        if (pathInst && pathInst->getDebugLoc()) {
          pathLoc = sarif::utils::createLocationFromInstruction(pathInst);

          // Add instruction as snippet if we have a specific instruction
          if (pathPoint.inst) {
            std::string pathInstStr;
            llvm::raw_string_ostream pathInstOS(pathInstStr);
            pathInstOS << *pathPoint.inst;
            pathLoc.snippet = pathInstOS.str();
          }
        }

        // Create thread flow location
        std::string message = pathPoint.description;
        if (message.empty() && pathPoint.bb) {
          message = "Execution reaches basic block in " +
                    (pathPoint.bb->getParent()
                         ? pathPoint.bb->getParent()->getName().str()
                         : "unknown");
        }

        sarif::ThreadFlowLocation tfl(pathLoc, message, order++);
        codeFlow.threadFlowLocations.push_back(tfl);
      }

      // Add the bug location as the final step in the path
      sarif::ThreadFlowLocation bugTfl(
          loc, "Bug detected: " + std::string(mkstr(bugType)), order);
      codeFlow.threadFlowLocations.push_back(bugTfl);

      result.codeFlows.push_back(codeFlow);
    }

    sarifLog.addResult(result);
  };

  // Add results for integer overflow bugs
  if (CheckIntOverflow) {
    for (auto *inst : overflow_insts) {
      addBugResult(inst, interr::INT_OVERFLOW);
    }
  }

  // Add results for division by zero bugs
  if (CheckDivByZero) {
    for (auto *inst : div_zero_insts) {
      addBugResult(inst, interr::DIV_BY_ZERO);
    }
  }

  // Add results for bad shift bugs
  if (CheckBadShift) {
    for (auto *inst : bad_shift_insts) {
      addBugResult(inst, interr::BAD_SHIFT);
    }
  }

  // Add results for array out of bounds bugs
  if (CheckArrayOOB) {
    for (auto *inst : gep_oob) {
      addBugResult(inst, interr::ARRAY_OOB);
    }
  }

  // Add results for dead branch bugs
  if (CheckDeadBranch) {
    for (const auto &pair : impossible_branches) {
      addBugResult(pair.first,
                   pair.second ? interr::DEAD_TRUE_BR : interr::DEAD_FALSE_BR);
    }
  }

  // Write SARIF output to file
  sarifLog.writeToFile(filename, true);

  MKINT_LOG() << "SARIF report written to: " << filename;
}

} // namespace kint

#include "Verification/SymAbsAI/Core/FloatingPointModel.h"

#include "Verification/SymAbsAI/Core/repr.h"

#include <sstream>

#include <z3_fpa.h>

namespace symabs_ai {
namespace {
class IEEEModel : public FloatingPointModel {
public:
  IEEEModel(const FunctionContext &fctx) : FloatingPointModel(fctx) {}

  virtual bool supportsType(llvm::Type *type) const override {
    // TODO: more than IEEE doubles
    return type->isDoubleTy();
  }

  virtual z3::sort sortForType(llvm::Type *type) const override {
    assert(supportsType(type));
    return z3::sort(Ctx_, Z3_mk_fpa_sort_double(Ctx_));
  }

  virtual z3::expr literal(llvm::ConstantFP *constant) const override {
    assert(supportsType(constant->getType()));
    auto apint = constant->getValueAPF().bitcastToAPInt();
    assert(apint.getBitWidth() == 64);

    uint64_t all_bits = apint.getLimitedValue();
    unsigned sign = (all_bits >> 63) & 0x1;              // one bit
    unsigned exponent = (all_bits >> 52) & 0x7ff;        // 11 bits
    uint64_t significand = all_bits & 0xfffffffffffffLL; // 52 bits

    z3::expr bv_sign = Ctx_.bv_val(sign, 1);
    z3::expr bv_exponent = Ctx_.bv_val(exponent, 11);
    z3::expr bv_significand = Ctx_.bv_val((uint64_t)significand, 52);

    // FIXME: The order of the arguments in the call to Z3_mk_fpa_fp does
    // not agree with the Z3 documentation (bv_significand is switched with
    // bv_exponent). Most likely a bug in the documentation.
    auto *res = Z3_mk_fpa_fp(Ctx_, bv_sign, bv_significand, bv_exponent);

    return to_expr(Ctx_, res);
  }

  z3::expr binopToExpr(llvm::BinaryOperator &binop, z3::expr in0,
                       z3::expr in1) const {
    Z3_ast ast;

    switch (binop.getOpcode()) {
    case llvm::Instruction::FAdd:
      ast = Z3_mk_fpa_add(Ctx_, roundingMode(), in0, in1);
      break;

    case llvm::Instruction::FSub:
      ast = Z3_mk_fpa_sub(Ctx_, roundingMode(), in0, in1);
      break;

    case llvm::Instruction::FMul:
      ast = Z3_mk_fpa_mul(Ctx_, roundingMode(), in0, in1);
      break;

    case llvm::Instruction::FDiv:
      ast = Z3_mk_fpa_div(Ctx_, roundingMode(), in0, in1);
      break;

    case llvm::Instruction::FRem:
      ast = Z3_mk_fpa_rem(Ctx_, in0, in1);
      break;

    default:
      llvm_unreachable("unknown floating point operator");
    }

    return to_expr(Ctx_, ast);
  }

  virtual z3::expr transferBinop(llvm::BinaryOperator &binop, z3::expr res,
                                 z3::expr in0, z3::expr in1) const override {
    return res == binopToExpr(binop, in0, in1);
  }

  virtual z3::expr transferCmp(llvm::CmpInst::Predicate pred, z3::expr res,
                               z3::expr in0, z3::expr in1) const override {
    using llvm::CmpInst;

    // If one of the arguments is NaN, an "ordered" comparison will always
    // return false while an "unordered" one will be true. See the LLVM
    // documentation for CmpInst::Predicate for details.
    bool is_ordered = false;

    z3::expr in0_is_nan(Ctx_, Z3_mk_fpa_is_nan(Ctx_, in0));
    z3::expr in1_is_nan(Ctx_, Z3_mk_fpa_is_nan(Ctx_, in1));
    z3::expr arg_is_nan = in0_is_nan || in1_is_nan;
    z3::expr res_true = (res == Ctx_.bv_val(1, 1));

    // Note that operators with ordered/unordered variant break from the
    // switch while others return. The ones that break are supposed to set
    // the variable `ast` and possibly set `is_ordered` to true.
    Z3_ast ast; // C-API boolean Z3 expression

    switch (pred) {
    case CmpInst::FCMP_FALSE:
      return !res_true;

    case CmpInst::FCMP_TRUE:
      return res_true;

    case CmpInst::FCMP_OEQ:
      is_ordered = true;
    case CmpInst::FCMP_UEQ:
      ast = Z3_mk_fpa_eq(Ctx_, in0, in1);
      break;

    case CmpInst::FCMP_OGT:
      is_ordered = true;
    case CmpInst::FCMP_UGT:
      ast = Z3_mk_fpa_gt(Ctx_, in0, in1);
      break;

    case CmpInst::FCMP_OGE:
      is_ordered = true;
    case CmpInst::FCMP_UGE:
      ast = Z3_mk_fpa_geq(Ctx_, in0, in1);
      break;

    case CmpInst::FCMP_OLT:
      is_ordered = true;
    case CmpInst::FCMP_ULT:
      ast = Z3_mk_fpa_lt(Ctx_, in0, in1);
      break;

    case CmpInst::FCMP_OLE:
      is_ordered = true;
    case CmpInst::FCMP_ULE:
      ast = Z3_mk_fpa_leq(Ctx_, in0, in1);
      break;

    case CmpInst::FCMP_ONE:
      is_ordered = true;
    case CmpInst::FCMP_UNE:
      ast = Z3_mk_not(Ctx_, Z3_mk_fpa_eq(Ctx_, in0, in1));
      break;

    case CmpInst::FCMP_ORD:
      return res_true == !arg_is_nan;

    case CmpInst::FCMP_UNO:
      return res_true == arg_is_nan;

    default:
      llvm_unreachable("unknown floating point comparison");
    }

    if (is_ordered) {
      return res_true == (to_expr(Ctx_, ast) && !arg_is_nan);
    } else {
      return res_true == (to_expr(Ctx_, ast) || arg_is_nan);
    }
  }

  virtual z3::expr transferConversion(z3::expr res, llvm::Instruction *inst,
                                      z3::expr in) const override {
    using llvm::Instruction;
    auto rm = roundingMode();
    Z3_ast ast;
    z3::sort out_sort = FunctionContext_.sortForType(inst->getType());

    switch (inst->getOpcode()) {
    case Instruction::FPToUI:
      ast = Z3_mk_fpa_to_ubv(Ctx_, rm, in, out_sort.bv_size());
      break;

    case Instruction::FPToSI:
      ast = Z3_mk_fpa_to_sbv(Ctx_, rm, in, out_sort.bv_size());
      break;

    case Instruction::UIToFP:
      ast = Z3_mk_fpa_to_fp_unsigned(Ctx_, rm, in, out_sort);
      break;

    case Instruction::SIToFP:
      ast = Z3_mk_fpa_to_fp_signed(Ctx_, rm, in, out_sort);
      break;

    case Instruction::FPTrunc:
    case Instruction::FPExt:
      // TODO: handle at least noop casts
      llvm_unreachable("not implemented");

    default:
      llvm_unreachable("invalid floating point conversion");
    }

    return res == to_expr(Ctx_, ast);
  }
};

/**
 * A memory model that attempts to take into account some of the weird behavior
 * of some compilers producing code using the x87 FPU.
 *
 * All operations are done in 80-bit precision but can be nondeterministically
 * rounded to their declared precision after every operation (simulating a spill
 * to memory from an x87 register).
 *
 * This doesn't model all of the possible weird behavior. In particular, even
 * an operation that doesn't modify a variable `x` can change it if it's spilled
 * to memory at this point. This breaks the SSA property of the intermediate
 * representation we can't model this at the moment.
 */
class X87Model : public FloatingPointModel {
private:
  IEEEModel IEEE_;
  mutable unsigned VarCount_;
  static constexpr const char *VAR_PREFIX = "__FP_SPILL_";

  /**
   * Rounds an expression to a given LLVM floating point type and then coverts
   * it back to an x87 extended float.
   */
  z3::expr simulateSpill(z3::expr expr, llvm::Type *type) const {
    z3::sort ieee_sort = IEEE_.sortForType(type);
    Z3_ast as_ieee =
        Z3_mk_fpa_to_fp_float(Ctx_, roundingMode(), expr, ieee_sort);
    Z3_ast spilled_expr =
        Z3_mk_fpa_to_fp_float(Ctx_, roundingMode(), as_ieee, expr.get_sort());
    return z3::expr(Ctx_, spilled_expr);
  }

  z3::expr possibleSpill(z3::expr expr, llvm::Type *type) const {
    std::ostringstream ssname;
    ssname << VAR_PREFIX << VarCount_;
    z3::expr dec_var = Ctx_.bool_const(ssname.str().c_str());
    return z3::ite(dec_var, simulateSpill(expr, type), expr);
  }

public:
  X87Model(const FunctionContext &fctx)
      : FloatingPointModel(fctx), IEEE_(fctx), VarCount_(0) {}

  virtual bool supportsType(llvm::Type *type) const override {
    return IEEE_.supportsType(type);
  }

  virtual z3::sort sortForType(llvm::Type *type) const override {
    // represent everything as an x87 80-bit float
    return z3::sort(Ctx_, Z3_mk_fpa_sort(Ctx_, 15, 64));
  }

  virtual z3::expr literal(llvm::ConstantFP *constant) const override {
    z3::expr ieee_lit = IEEE_.literal(constant);
    Z3_ast ast = Z3_mk_fpa_to_fp_float(Ctx_, roundingMode(), ieee_lit,
                                       sortForType(constant->getType()));
    return z3::expr(Ctx_, ast);
  }

  virtual z3::expr transferBinop(llvm::BinaryOperator &binop, z3::expr res,
                                 z3::expr in0, z3::expr in1) const override {
    z3::expr exact_res = IEEE_.binopToExpr(binop, in0, in1);
    return res == possibleSpill(exact_res, binop.getType());
  }

  virtual z3::expr transferCmp(llvm::CmpInst::Predicate pred, z3::expr res,
                               z3::expr in0, z3::expr in1) const override {
    return transferCmp(pred, res, in0, in1);
  }

  virtual z3::expr transferConversion(z3::expr res, llvm::Instruction *inst,
                                      z3::expr in) const override {
    // FIXME: is this really correct?
    return IEEE_.transferConversion(res, inst, in);
  }
};
} // namespace

z3::expr FloatingPointModel::roundingMode() const {
  auto rmode = FunctionContext_.getConfig().get<std::string>(
      "FloatingPointModel", "Rounding", "NearestTiesToEven");

  if (rmode == "NearestTiesToEven")
    return to_expr(Ctx_, Z3_mk_fpa_round_nearest_ties_to_even(Ctx_));
  else if (rmode == "NearestTiesToAway")
    return to_expr(Ctx_, Z3_mk_fpa_round_nearest_ties_to_away(Ctx_));
  else if (rmode == "TowardPositive")
    return to_expr(Ctx_, Z3_mk_fpa_round_toward_positive(Ctx_));
  else if (rmode == "TowardNegative")
    return to_expr(Ctx_, Z3_mk_fpa_round_toward_negative(Ctx_));
  else if (rmode == "TowardZero")
    return to_expr(Ctx_, Z3_mk_fpa_round_toward_zero(Ctx_));
  else {
    if (rmode == "Nondeterministic")
      RoundingModeCounter_++;

    std::ostringstream name;
    name << RM_PREFIX << RoundingModeCounter_;
    z3::sort rm_sort(Ctx_, Z3_mk_fpa_rounding_mode_sort(Ctx_));
    return Ctx_.constant(name.str().c_str(), rm_sort);
  }
}

bool FloatingPointModel::supportsType(llvm::Type *) const { return false; }

z3::sort FloatingPointModel::sortForType(llvm::Type *) const {
  llvm_unreachable("must never be called");
}

z3::expr FloatingPointModel::literal(llvm::ConstantFP *) const {
  llvm_unreachable("must never be called");
}

unique_ptr<FloatingPointModel>
FloatingPointModel::New(const FunctionContext &fctx) {
  auto variant = fctx.getConfig().get<std::string>("FloatingPointModel",
                                                   "Variant", "None");

  if (variant == "None")
    return unique_ptr<FloatingPointModel>(new FloatingPointModel(fctx));
  else if (variant == "IEEE")
    return unique_ptr<FloatingPointModel>(new IEEEModel(fctx));
  else if (variant == "X87")
    return unique_ptr<FloatingPointModel>(new X87Model(fctx));
  else
    panic("incorrect floating point model");
}
} // namespace symabs_ai

/**
 * @file InstructionSemantics.cpp
 * @brief Implementation of SMT encoding for LLVM instruction semantics.
 *
 * InstructionSemantics translates LLVM IR instructions into Z3 SMT formulas
 * that encode their semantics. It handles arithmetic operations, memory
 * operations, control flow, floating-point operations, and various
 * LLVM-specific constructs. The visitor pattern is used to dispatch to
 * instruction-specific encoders.
 *
 * @author rainoftime
 */
#include "Verification/SymAbsAI/Core/InstructionSemantics.h"

#include "Verification/SymAbsAI/Core/FloatingPointModel.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <sstream>

#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstrTypes.h>
#include <z3_fpa.h>

namespace // anonymous
{
/**
 * @brief Construct a condition that is true iff a right shift is exact (no bits
 * shifted out).
 *
 * For a right shift `_shr(in0, in1)`, this checks that all bits that would be
 * shifted out are zero. This is used to encode the "exact" flag on LLVM shift
 * instructions.
 *
 * @param ctx Z3 context for creating expressions
 * @param in0 The value being shifted
 * @param in1 The shift amount
 * @return Z3 expression that is true iff the shift is exact
 */
z3::expr getShrExactCondition(z3::context *const ctx, const z3::expr in0,
                              const z3::expr in1) {
  unsigned int bw0 = in0.get_sort().bv_size();

  z3::expr zero = ctx->bv_val(0, bw0);
  z3::expr mask = ~z3_ext::shl(zero - 1, in1);
  // only set the bits that would be shifted out
  z3::expr shifted_out = in0 & mask;

  return (shifted_out == zero);
  // the shifted-out bits have to be all zeros
}

} // namespace

namespace symabs_ai {
/**
 * @brief Construct an InstructionSemantics visitor for a fragment.
 *
 * @param fctx Function context providing Z3 context and memory/floating-point
 * models
 * @param frag Fragment containing the instructions to encode
 */
InstructionSemantics::InstructionSemantics(const FunctionContext &fctx,
                                           const Fragment &frag)
    : FunctionContext_(fctx), Fragment_(frag), Z3Context_(&fctx.getZ3()) {}

/**
 * @brief Check if a value's type is supported for SMT encoding.
 *
 * Supports integer types, pointer types, and floating-point types (if the
 * floating-point model supports them). Vector types are not supported.
 *
 * @param val The LLVM value to check
 * @return true if the type is supported, false otherwise
 */
bool InstructionSemantics::hasSupportedType(llvm::Value *val) {
  llvm::Type *tp = val->getType();

  if (tp->isFloatingPointTy()) {
    return FunctionContext_.getFloatingPointModel().supportsType(tp);
  }

  return !tp->isVectorTy();
}

/**
 * @brief Check if an instruction has valid operands for SMT encoding.
 *
 * Validates that both the instruction itself and all its operands have
 * supported types. Used to filter out unsupported instructions before
 * attempting to encode them.
 *
 * @param instr The instruction to validate
 * @return true if all operands are valid, false otherwise
 */
bool InstructionSemantics::hasValidOperands(llvm::Instruction &instr) {
  if (!hasSupportedType(&instr)) {
    vout << "Instruction " << repr(instr) << " has unsupported type." << '\n';
    return false;
  }

  for (auto &op : instr.operands()) {
    if (!hasSupportedType(op.get())) {
      vout << "Instruction '" << repr(instr)
           << "' uses arguments with unsupported type." << '\n';
      return false;
    }
  }

  return true;
}

/**
 * @brief Get the Z3 expression representing the r-value (read value) of an LLVM
 * value.
 *
 * Handles multiple cases:
 * - Represented variables: returns their SMT variable from ValueMapping
 * - Constant integers: converts to Z3 bitvector constant
 * - Constant floating-point: converts using FloatingPointModel
 * - Constant pointer null: returns nullptr from MemoryModel
 * - Constant expressions: recursively encodes as instructions
 * - Other values: creates a fresh unconstrained variable
 *
 * @param value The LLVM value to encode
 * @return Z3 expression representing the value
 */
z3::expr InstructionSemantics::rValue(llvm::Value *value) {
  // represented variable
  if (FunctionContext_.isRepresentedValue(value)) {
    ValueMapping vmap =
        ValueMapping::before(FunctionContext_, Fragment_, Instruction_);
    return vmap.getFullRepresentation(value);
  }

  // constant int
  if (auto *val = llvm::dyn_cast<llvm::ConstantInt>(value)) {
    return makeConstantInt(Z3Context_, val);
  }

  // constant float
  if (FunctionContext_.getFloatingPointModel().supportsType(value->getType())) {
    if (auto *as_constfp = llvm::dyn_cast<llvm::ConstantFP>(value))
      return FunctionContext_.getFloatingPointModel().literal(as_constfp);
  }

  // constant pointer value ``null''
  if (llvm::isa<llvm::ConstantPointerNull>(value)) {
    return FunctionContext_.getMemoryModel().make_nullptr();
  }

  // constant expression
  if (auto *as_cexpr = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    // We reuse the machinery of InstructionSemantics to handle constant
    // expressions using visit* methods for corresponding instructions.
    // Because visit* produces a boolean expression we need to make up a
    // variable name for the expression and add the result of visit to
    // InstructionAssumptions_
    // Note: getAsInstruction() creates a new instruction that we must manually
    // delete
    llvm::Instruction *as_instr = as_cexpr->getAsInstruction();

    // make up a (hopefully) unique variable name
    std::ostringstream name;
    name << "__CONSTEXPR_" << std::hex << (uintptr_t)as_cexpr;
    as_instr->setName(name.str().c_str());

    if (!InstructionAssumptions_)
      InstructionAssumptions_.reset(new z3::expr(Z3Context_->bool_val(1)));

    // generate a formula using a temporary InstructionSemantics
    InstructionSemantics temp_sem(FunctionContext_, Fragment_);
    InstructionAssumptions_.reset(
        new z3::expr(*InstructionAssumptions_ && temp_sem.visit(*as_instr)));

    // Manually delete the instruction since it's heap-allocated
    as_instr->deleteValue();

    return Z3Context_->constant(name.str().c_str(),
                                FunctionContext_.sortForType(value->getType()));
  }

  // Not a represented variable and not a constant. This means that this is a
  // value that we either chose not to represent as an SMT variable or it's
  // an Instruction we did not implement. In such case we return a bv_const
  // with a unique name.
  std::string name;
  if (value->getName().str().length() > 0) {
    name = value->getName().str();
  } else {
    // make up a name based on the address of value
    std::ostringstream out;
    out << "__UNKNOWN_" << std::ios::hex << (uintptr_t)value;
    name = out.str();
  }

  return Z3Context_->constant(name.c_str(),
                              FunctionContext_.sortForType(value->getType()));
}

/**
 * @brief Get the Z3 expression representing the l-value (write target) of the
 * current instruction.
 *
 * Returns the SMT variable that the current instruction writes to. For
 * temporary instructions (e.g., from ConstantExpr), uses the instruction's name
 * directly. Otherwise, uses ValueMapping to get the primed version of the
 * instruction's result.
 *
 * @return Z3 expression representing the l-value (must be called while visiting
 * an instruction)
 */
z3::expr InstructionSemantics::lValue() {
  if (Instruction_->getParent() == nullptr) {
    // Happens for temporary instructions created for ConstantExprs. See
    // rValue() for details on how constant expressions are handled.
    z3::sort sort = FunctionContext_.sortForType(Instruction_->getType());
    return Z3Context_->constant(Instruction_->getName().str().c_str(), sort);
  }

  if (!FunctionContext_.isRepresentedValue(Instruction_)) {
    // TODO: Instead of making up a name, just don't emit any formulas for
    // unrepresented instructions.
    std::ostringstream out;
    out << "__UNKNOWN_" << std::ios::hex << (uintptr_t)Instruction_;
    return Z3Context_->constant(
        out.str().c_str(),
        FunctionContext_.sortForType(Instruction_->getType()));
  }

  ValueMapping vmap =
      ValueMapping::after(FunctionContext_, Fragment_, Instruction_);
  return vmap.getFullRepresentation(Instruction_);
}

// This is called *before* all other visit* methods and can filter out some
// kinds of instructions.
z3::expr InstructionSemantics::visit(llvm::Instruction &I) {
  using llvm::Instruction;

  Instruction_ = &I;
  z3::expr result = Z3Context_->bool_val(true);

  static const std::set<unsigned> fp_convs = {
      Instruction::FPToUI, Instruction::FPToSI,  Instruction::UIToFP,
      Instruction::SIToFP, Instruction::FPTrunc, Instruction::FPExt};

  if (hasValidOperands(I)) {
    // floating point conversions are handled all together
    if (fp_convs.find(I.getOpcode()) != fp_convs.end()) {
      z3::expr input = rValue(I.getOperand(0));
      result = FunctionContext_.getFloatingPointModel().transferConversion(
          lValue(), &I, input);
    } else {
      // dispatch to individual visit* methods
      result = llvm::InstVisitor<InstructionSemantics, z3::expr>::visit(I);
    }
  }

  if (InstructionAssumptions_)
    result = result && *InstructionAssumptions_;

  return result;
}

// Terminator Instructions
z3::expr InstructionSemantics::visitRet(llvm::ReturnInst &I) {
  // No additional constraints for the semantics from a return inst
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitBr(llvm::BranchInst &I) {
  if (I.isConditional()) {
    assert(I.getNumSuccessors() == 2);
    z3::expr cond_var = rValue(I.getCondition());

    // cond_var might be a 1-bit bitvector
    if (cond_var.is_bv())
      cond_var = cond_var != Z3Context_->bv_val(0, 1);

    llvm::BasicBlock *true_bb = I.getSuccessor(0);
    llvm::BasicBlock *false_bb = I.getSuccessor(1);

    const std::set<Fragment::edge> &edges = Fragment_.edges();
    bool has_true = edges.find({I.getParent(), true_bb}) != edges.end();
    bool has_false = edges.find({I.getParent(), false_bb}) != edges.end();

    if (has_true || has_false) {
      z3::expr takes_true = Z3Context_->bool_val(false);
      if (has_true) {
        takes_true = FunctionContext_.getEdgeVariable(I.getParent(), true_bb);
      }

      z3::expr takes_false = Z3Context_->bool_val(false);
      if (has_false) {
        takes_false = FunctionContext_.getEdgeVariable(I.getParent(), false_bb);
      }

      return implies(cond_var, takes_true) && implies(!cond_var, takes_false);
    } else {
      return Z3Context_->bool_val(true);
    }
  } else {
    assert(I.getNumSuccessors() == 1);
    return Z3Context_->bool_val(true);
  }
}

z3::expr InstructionSemantics::visitSwitch(llvm::SwitchInst &I) {
  z3::expr val = rValue(I.getCondition());
  z3::expr result = Z3Context_->bool_val(true);
  z3::expr not_default_cond = Z3Context_->bool_val(false);

  for (auto case_it = I.case_begin(), end = I.case_end(); case_it != end;
       ++case_it) {
    z3::expr cmp = rValue(case_it->getCaseValue());
    z3::expr succ_edge = FunctionContext_.getEdgeVariable(
        I.getParent(), case_it->getCaseSuccessor());

    result = result && implies((val == cmp), succ_edge);
    not_default_cond = not_default_cond || (val == cmp);
  }
  z3::expr default_edge =
      FunctionContext_.getEdgeVariable(I.getParent(), I.getDefaultDest());
  result = result && implies(!not_default_cond, default_edge);

  return result;
}

z3::expr InstructionSemantics::visitIndirectBr(llvm::IndirectBrInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitInvoke(llvm::InvokeInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitResume(llvm::ResumeInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitUnreachable(llvm::UnreachableInst &I) {
  // No additional constraints for the semantics from an unreachable inst
  return Z3Context_->bool_val(true);
}

// Arithmetic Instructions

z3::expr InstructionSemantics::visitAdd(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  if (I.hasNoSignedWrap()) {
    cond = cond && z3_ext::add_nof(in0, in1, true);
  }
  if (I.hasNoUnsignedWrap()) {
    cond = cond && z3_ext::add_nof(in0, in1, false);
  }
  if (I.hasNoSignedWrap() || I.hasNoUnsignedWrap()) {
    cond = cond && z3_ext::add_nuf(in0, in1);
  }

  z3::expr result = out_var == in0 + in1;

  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitFAdd(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return FunctionContext_.getFloatingPointModel().transferBinop(I, out_var, in0,
                                                                in1);
}

z3::expr InstructionSemantics::visitSub(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  if (I.hasNoSignedWrap()) {
    cond = cond && z3_ext::sub_nuf(in0, in1, true);
  }
  if (I.hasNoUnsignedWrap()) {
    cond = cond && z3_ext::sub_nuf(in0, in1, false);
  }
  if (I.hasNoSignedWrap() || I.hasNoUnsignedWrap()) {
    cond = cond && z3_ext::sub_nof(in0, in1);
  }

  z3::expr result = out_var == in0 - in1;

  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitFSub(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return FunctionContext_.getFloatingPointModel().transferBinop(I, out_var, in0,
                                                                in1);
}

z3::expr InstructionSemantics::visitMul(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  if (I.hasNoSignedWrap()) {
    cond = cond && z3_ext::mul_nof(in0, in1, true);
  }
  if (I.hasNoUnsignedWrap()) {
    cond = cond && z3_ext::mul_nof(in0, in1, false);
  }
  if (I.hasNoSignedWrap() || I.hasNoUnsignedWrap()) {
    cond = cond && z3_ext::mul_nuf(in0, in1);
  }

  z3::expr result = out_var == in0 * in1;

  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitFMul(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return FunctionContext_.getFloatingPointModel().transferBinop(I, out_var, in0,
                                                                in1);
}

z3::expr InstructionSemantics::visitUDiv(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  z3::expr zero =
      Z3Context_->bv_val(0, I.getOperand(1)->getType()->getIntegerBitWidth());
  cond = cond && (in1 != zero);

  if (I.isExact()) {
    z3::expr zero =
        Z3Context_->bv_val(0, I.getOperand(1)->getType()->getIntegerBitWidth());
    cond = cond && (z3_ext::urem(in0, in1) == zero);
  }

  z3::expr result = out_var == udiv(in0, in1);
  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitSDiv(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  cond =
      cond && (in1 != Z3Context_->bv_val(
                          0, I.getOperand(1)->getType()->getIntegerBitWidth()));
  cond = cond && z3_ext::sdiv_nof(in0, in1);
  if (I.isExact()) {
    cond = cond && (z3_ext::srem(in0, in1) ==
                    Z3Context_->bv_val(
                        0, I.getOperand(1)->getType()->getIntegerBitWidth()));
  }

  z3::expr result = out_var == in0 / in1;
  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitFDiv(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return FunctionContext_.getFloatingPointModel().transferBinop(I, out_var, in0,
                                                                in1);
}

z3::expr InstructionSemantics::visitURem(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  cond =
      cond && (in1 != Z3Context_->bv_val(
                          0, I.getOperand(1)->getType()->getIntegerBitWidth()));

  z3::expr result = out_var == z3_ext::urem(in0, in1);
  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitSRem(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  z3::expr cond = Z3Context_->bool_val(true);
  cond =
      cond && (in1 != Z3Context_->bv_val(
                          0, I.getOperand(1)->getType()->getIntegerBitWidth()));
  cond = cond && z3_ext::sdiv_nof(in0, in1);

  z3::expr result = out_var == z3_ext::srem(in0, in1);
  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitFRem(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return FunctionContext_.getFloatingPointModel().transferBinop(I, out_var, in0,
                                                                in1);
}

// Bit Operations

z3::expr InstructionSemantics::visitShl(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  unsigned int bw0 = in0.get_sort().bv_size();
  unsigned int bw1 = in1.get_sort().bv_size();

  // shiftwidth >= bitwidth yields undefined behavior
  z3::expr cond = ult(in1, Z3Context_->bv_val(bw0, bw1));

  if (I.hasNoSignedWrap() || I.hasNoUnsignedWrap()) {
    z3::expr zero = Z3Context_->bv_val(0, bw0);
    z3::expr low_mask = z3_ext::lshr(zero - 1, in1);
    z3::expr mask = ~low_mask;
    // only set the bits that would be shifted out
    z3::expr shifted_out = in0 & mask;

    if (I.hasNoUnsignedWrap()) {
      cond = cond && (shifted_out == zero);
      // the shifted-out bits have to be all zeros
    }
    if (I.hasNoSignedWrap()) {
      // create a mask with only the future signbit set
      z3::expr signbit_mask =
          z3_ext::lshr(mask, Z3Context_->bv_val(1, bw0)) & low_mask;
      // if the resulting sign bit is not set, this is zero, otherwise
      // it is a bitvector with all the shifted-out bits set
      z3::expr cmp_val = ite((in0 & signbit_mask) == zero, zero, mask);
      cond = cond && (shifted_out == cmp_val);
      // the shifted-out bits have to be all the same as the resulting
      // sign bit
    }
  }

  z3::expr result = out_var == z3_ext::shl(in0, in1);

  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitLShr(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  unsigned int bw0 = in0.get_sort().bv_size();
  unsigned int bw1 = in1.get_sort().bv_size();

  // shiftwidth >= bitwidth yields undefined behavior
  z3::expr cond = ult(in1, Z3Context_->bv_val(bw0, bw1));

  if (I.isExact()) {
    cond = cond && getShrExactCondition(Z3Context_, in0, in1);
  }

  z3::expr result = out_var == z3_ext::lshr(in0, in1);

  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitAShr(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  unsigned int bw0 = in0.get_sort().bv_size();
  unsigned int bw1 = in1.get_sort().bv_size();

  // shiftwidth >= bitwidth yields undefined behavior
  z3::expr cond = ult(in1, Z3Context_->bv_val(bw0, bw1));

  if (I.isExact()) {
    cond = cond && getShrExactCondition(Z3Context_, in0, in1);
  }

  z3::expr result = out_var == z3_ext::ashr(in0, in1);

  return implies(cond, result) &&
         implies(!cond, FunctionContext_.getUndefinedBehaviorFlag());
}

z3::expr InstructionSemantics::visitAnd(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return out_var == (in0 & in1);
}

z3::expr InstructionSemantics::visitOr(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return out_var == (in0 | in1);
}

z3::expr InstructionSemantics::visitXor(llvm::BinaryOperator &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return out_var == (in0 ^ in1);
  // In C++, the operator == has higher precedence than ^.
}

// Memory Access Instructions

z3::expr InstructionSemantics::allocationFormula(z3::expr size) {
  const MemoryModel &mm(FunctionContext_.getMemoryModel());

  z3::expr out_var = lValue();

  auto vm_before =
      ValueMapping::before(FunctionContext_, Fragment_, Instruction_);
  auto vm_after =
      ValueMapping::after(FunctionContext_, Fragment_, Instruction_);

  return mm.allocate(vm_before.memory(), vm_after.memory(), out_var, size);
}

z3::expr InstructionSemantics::visitAlloca(llvm::AllocaInst &I) {
  llvm::Type *type = I.getAllocatedType();

  llvm::DataLayout *dl = FunctionContext_.getModuleContext().getDataLayout();
  int bytes = dl->getTypeAllocSize(type);

  z3::expr num_obj = adjustBitwidth(rValue(I.getArraySize()),
                                    FunctionContext_.getPointerSize());

  z3::expr size = num_obj * bytes;

  return allocationFormula(size);
}

z3::expr InstructionSemantics::visitLoad(llvm::LoadInst &I) {
  if (I.isVolatile()) {
    // Nothing can be assumed about results of volatile loads.
    return Z3Context_->bool_val(true);
  }

  const MemoryModel &mm(FunctionContext_.getMemoryModel());
  auto vm = ValueMapping::before(FunctionContext_, Fragment_, Instruction_);
  return mm.load(lValue(), vm.memory(), rValue(I.getOperand(0)));
}

z3::expr InstructionSemantics::visitStore(llvm::StoreInst &I) {
  if (I.isVolatile()) {
    // Result of a volatile store can be only read with a volatile loads
    // which is allowed to return anything. No need to really keep track
    // of the stored value.
    return Z3Context_->bool_val(true);
  }

  const MemoryModel &mm(FunctionContext_.getMemoryModel());
  auto vm_before =
      ValueMapping::before(FunctionContext_, Fragment_, Instruction_);
  auto vm_after =
      ValueMapping::after(FunctionContext_, Fragment_, Instruction_);
  z3::expr val = rValue(I.getOperand(0));
  z3::expr addr = rValue(I.getOperand(1));
  return mm.store(vm_before.memory(), vm_after.memory(), addr, val);
}

z3::expr InstructionSemantics::visitGetElementPtr(llvm::GetElementPtrInst &I) {
  using namespace llvm;

  int ptr_size = FunctionContext_.getPointerSize();
  Type *type = I.getPointerOperand()->getType();
  z3::expr value = rValue(I.getPointerOperand());
  z3::expr offset_expr = FunctionContext_.getZ3().bv_val(0, ptr_size);
  llvm::DataLayout *dl = FunctionContext_.getModuleContext().getDataLayout();

  for (auto *itr = I.op_begin() + 1; itr != I.op_end(); ++itr) {
    if (StructType *type_st = dyn_cast<StructType>(type)) {
      uint64_t field_id = cast<ConstantInt>(*itr)->getZExtValue();
      type = type_st->getElementType(field_id);
      uint64_t offset =
          dl->getStructLayout(type_st)->getElementOffset(field_id);
      offset_expr =
          offset_expr + Z3Context_->bv_val((uint64_t)offset, ptr_size);
    } else {
      // In LLVM 12/14, need to check for specific sequential type
      if (type->isArrayTy())
        type = type->getArrayElementType();
      else if (type->isPointerTy())
        type = type->getPointerElementType();
      else if (type->isVectorTy())
        type = llvm::cast<llvm::VectorType>(type)->getElementType();

      uint64_t element_size = dl->getTypeAllocSize(type);

      z3::expr idx = rValue(*itr);
      int idx_bv = idx.get_sort().bv_size();
      z3::expr offset =
          idx * Z3Context_->bv_val((uint64_t)element_size, idx_bv);

      if (idx_bv < ptr_size) {
        offset_expr = offset_expr + z3_ext::zext(ptr_size - idx_bv, offset);
      } else if (idx_bv > ptr_size) {
        llvm_unreachable("not implemented"); // FIXME
      } else {
        offset_expr = offset_expr + offset;
      }
    }
  }

  return FunctionContext_.getMemoryModel().getelementptr(lValue(), value,
                                                         offset_expr);
}

z3::expr InstructionSemantics::visitFence(llvm::FenceInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitAtomicCmpXchg(llvm::AtomicCmpXchgInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitAtomicRMW(llvm::AtomicRMWInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

// Conversion Instructions

z3::expr InstructionSemantics::visitTrunc(llvm::TruncInst &I) {
  unsigned int to_size = I.getDestTy()->getIntegerBitWidth();
  z3::expr in = rValue(I.getOperand(0));
  z3::expr out_var = lValue();

  return out_var == adjustBitwidth(in, to_size);
}

z3::expr InstructionSemantics::visitZExt(llvm::ZExtInst &I) {
  unsigned int to_size = I.getDestTy()->getIntegerBitWidth();
  z3::expr in = rValue(I.getOperand(0));
  z3::expr out_var = lValue();

  return out_var == adjustBitwidth(in, to_size);
}

z3::expr InstructionSemantics::visitSExt(llvm::SExtInst &I) {
  unsigned int from_size = I.getSrcTy()->getIntegerBitWidth();
  unsigned int to_size = I.getDestTy()->getIntegerBitWidth();
  z3::expr in = rValue(I.getOperand(0));
  z3::expr out_var = lValue();
  return out_var == z3_ext::sext(to_size - from_size, in);
}

z3::expr InstructionSemantics::visitFPToUI(llvm::FPToUIInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitFPToSI(llvm::FPToSIInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitUIToFP(llvm::UIToFPInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitSIToFP(llvm::SIToFPInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitFPTrunc(llvm::FPTruncInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitFPExt(llvm::FPExtInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitPtrToInt(llvm::PtrToIntInst &I) {
  const MemoryModel &mm(FunctionContext_.getMemoryModel());

  unsigned int to_size = I.getDestTy()->getIntegerBitWidth();
  z3::expr in = rValue(I.getPointerOperand());
  z3::expr out_var = lValue();

  return mm.ptrtoint(out_var, in, to_size);
}

z3::expr InstructionSemantics::visitIntToPtr(llvm::IntToPtrInst &I) {
  const MemoryModel &mm(FunctionContext_.getMemoryModel());

  z3::expr in = rValue(I.getOperand(0));
  z3::expr out_var = lValue();

  return mm.inttoptr(out_var, in);
}

z3::expr InstructionSemantics::visitBitCast(llvm::BitCastInst &I) {
  // According to the LLVM language reference, bitcast only operates on
  // non-aggregate first class types, i.e.:
  //
  //     * integers
  //     * floats (not represented by SymAbsAI)
  //     * X86_mmx (not represented)
  //     * pointers
  //     * vectors (not represented)
  //
  // Moreover, conversion between pointers and other types is not allowed
  // (inttoptr and ptrtoint serve this purpose) and all conversions must
  // be lossless. This leaves two cases: conversions between different
  // pointer types and a no-op conversion from n-bit int to an n-bit int.
  auto *src_ty = I.getSrcTy();
  auto *dst_ty = I.getDestTy();

  if (src_ty->isPointerTy() && dst_ty->isPointerTy())
    return lValue() == rValue(I.getOperand(0));

  if (src_ty->isIntegerTy() && dst_ty->isIntegerTy()) {
    assert(src_ty->getIntegerBitWidth() == dst_ty->getIntegerBitWidth());
    return lValue() == rValue(I.getOperand(0));
  }

  assert(!FunctionContext_.isRepresentedValue(&I) ||
         !FunctionContext_.isRepresentedValue(I.getOperand(0)));
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitAddrSpaceCast(llvm::AddrSpaceCastInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

// "Other" Instructions

z3::expr InstructionSemantics::visitICmp(llvm::ICmpInst &I) {
  using namespace llvm;

  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();

  switch (I.getPredicate()) {
  case CmpInst::ICMP_EQ:
    return (in0 == in1) == (out_var == Z3Context_->bv_val(1, 1));
  // return implies(in0 == in1, out_var == Z3Context_->bv_val(1,1))
  //     && implies(!(in0 == in1), out_var == Z3Context_->bv_val(0,1));
  case CmpInst::ICMP_NE:
    return (in0 != in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_UGT:
    return ugt(in0, in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_UGE:
    return uge(in0, in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_ULT:
    return ult(in0, in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_ULE:
    return ule(in0, in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_SGT: // z3 default is signed!
    return (in0 > in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_SGE:
    return (in0 >= in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_SLT:
    return (in0 < in1) == (out_var == Z3Context_->bv_val(1, 1));
  case CmpInst::ICMP_SLE:
    return (in0 <= in1) == (out_var == Z3Context_->bv_val(1, 1));
  default:
    llvm_unreachable("Unknown ICMP predicate!");
  }

  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitFCmp(llvm::FCmpInst &I) {
  z3::expr in0 = rValue(I.getOperand(0));
  z3::expr in1 = rValue(I.getOperand(1));
  z3::expr out_var = lValue();
  return FunctionContext_.getFloatingPointModel().transferCmp(
      I.getPredicate(), out_var, in0, in1);
}

z3::expr InstructionSemantics::visitPHI(llvm::PHINode &I) {
  z3::expr result = Z3Context_->bool_val(true);

  for (int i = 0; i < (int)I.getNumIncomingValues(); i++) {
    llvm::BasicBlock *bb_from = I.getIncomingBlock(i);

    auto &locs = Fragment_.locations();
    if (locs.find(bb_from) != locs.end()) {
      z3::expr edge_var =
          FunctionContext_.getEdgeVariable(bb_from, I.getParent());
      z3::expr in_expr = rValue(I.getIncomingValue(i));
      result = result && implies(edge_var, lValue() == in_expr);
    }
  }

  return result;
}

z3::expr InstructionSemantics::visitCall(llvm::CallInst &I) {
  auto *callee = I.getCalledFunction();
  if (callee == nullptr)
    return Z3Context_->bool_val(true);

  auto *tli = FunctionContext_.getModuleContext().getTargetLibraryInfo();
  // Check for malloc-like functions manually since isMallocLikeFn was removed
  // in LLVM 12
  bool is_malloc = callee && (callee->getName() == "malloc" ||
                              callee->getName() == "calloc" ||
                              callee->getName() == "realloc");
  if (is_malloc) {
    auto size = adjustBitwidth(rValue(I.getArgOperand(0)),
                               FunctionContext_.getPointerSize());
    return allocationFormula(size);
  }
  if (llvm::isFreeCall(&I, tli)) {
    const MemoryModel &mm(FunctionContext_.getMemoryModel());

    auto vm_before =
        ValueMapping::before(FunctionContext_, Fragment_, Instruction_);
    auto vm_after =
        ValueMapping::after(FunctionContext_, Fragment_, Instruction_);

    auto ptr = rValue(I.getArgOperand(0));

    return mm.deallocate(vm_before.memory(), vm_after.memory(), ptr);
  }

  auto &mctx = FunctionContext_.getModuleContext();
  z3::expr raw_expr = mctx.formulaFor(callee);
  z3::expr_vector src(*Z3Context_), dst(*Z3Context_);

  // substitute arguments
  auto *formal_itr = callee->arg_begin();
  auto *formal_end = callee->arg_end();
  auto *actual_itr = I.operands().begin();
  auto *actual_end = I.operands().end();

  for (; formal_itr != formal_end && actual_itr != actual_end;
       ++formal_itr, ++actual_itr) {

    // skip metadata arguments
    if ((*actual_itr)->getType()->isMetadataTy())
      continue;

    auto actual_rval = rValue(*actual_itr);
    src.push_back(Z3Context_->constant(formal_itr->getName().str().c_str(),
                                       actual_rval.get_sort()));
    dst.push_back(actual_rval);
  }

  // substitute return value
  if (!I.getType()->isVoidTy()) {
    src.push_back(
        Z3Context_->constant(mctx.getReturnSymbol(), rValue(&I).get_sort()));
    dst.push_back(lValue());
  }

  z3::expr result = raw_expr.substitute(src, dst);

  // if llvm knows that a function does not write to memory, make use of this
  if (callee->onlyReadsMemory() || callee->doesNotAccessMemory() ||
      I.getMetadata("symbolic_abstraction")) {
    auto vm_before =
        ValueMapping::before(FunctionContext_, Fragment_, Instruction_);
    auto vm_after =
        ValueMapping::after(FunctionContext_, Fragment_, Instruction_);
    result = result && (vm_before.memory() == vm_after.memory());
  }

  return result;
}

z3::expr InstructionSemantics::visitSelect(llvm::SelectInst &I) {
  z3::expr cond = rValue(I.getCondition());
  z3::expr true_val = rValue(I.getTrueValue());
  z3::expr false_val = rValue(I.getFalseValue());
  z3::expr out_var = lValue();
  z3::expr result =
      implies(cond == Z3Context_->bv_val(1, 1), out_var == true_val) &&
      implies(cond != Z3Context_->bv_val(1, 1), out_var == false_val);
  return result;
}

// visitUserOp1 and visitUserOp2 are now defined inline in the header file

z3::expr InstructionSemantics::visitVAArg(llvm::VAArgInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

// Vector Instructions

z3::expr
InstructionSemantics::visitExtractElement(llvm::ExtractElementInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitInsertElement(llvm::InsertElementInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitShuffleVector(llvm::ShuffleVectorInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

// Aggregate Instructions

z3::expr InstructionSemantics::visitExtractValue(llvm::ExtractValueInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitInsertValue(llvm::InsertValueInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

// Landing Pad Instruction

z3::expr InstructionSemantics::visitLandingPad(llvm::LandingPadInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

// LLVM 12/14 support cleanup and catch instructions
z3::expr
InstructionSemantics::visitCleanupReturnInst(llvm::CleanupReturnInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitCatchReturnInst(llvm::CatchReturnInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitCatchSwitchInst(llvm::CatchSwitchInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitFuncletPadInst(llvm::FuncletPadInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitCleanupPadInst(llvm::CleanupPadInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitCatchPadInst(llvm::CatchPadInst &I) {
  // TODO Implement this.
  std::cerr << "Skipping instruction '"
            << llvm::Instruction::getOpcodeName(I.getOpcode())
            << "'. Not yet implemented." << '\n';
  return Z3Context_->bool_val(true);
}

z3::expr InstructionSemantics::visitFreezeInst(llvm::FreezeInst &I) {
  // Freeze instruction - just preserve for now
  return preserve(I);
}

z3::expr InstructionSemantics::visitFNeg(llvm::UnaryOperator &I) {
  z3::expr in = rValue(I.getOperand(0));
  z3::expr out_var = lValue();
  Z3_ast ast = Z3_mk_fpa_neg(*Z3Context_, in);
  return out_var == to_expr(*Z3Context_, ast);
}
} // namespace symabs_ai

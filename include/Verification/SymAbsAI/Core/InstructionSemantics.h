#pragma once
/**
 * @file InstructionSemantics.h
 * @Brief Class for generating SMT constraints capturing the semantics of
 * arbitrary LLVM instructions.
 */

#include "Verification/SymAbsAI/Utils/Utils.h"

#include <set>

#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/Instructions.h>
#include <z3++.h>

namespace symabs_ai {

class FunctionContext;
class Fragment;

class InstructionSemantics
    : public llvm::InstVisitor<InstructionSemantics, z3::expr> {
private:
  const FunctionContext &FunctionContext_;
  const Fragment &Fragment_;
  z3::context *Z3Context_;

  // Currently processed instruction and additional conditions generated
  // about this instruction in visit* methods. See visit(Instruction&) for
  // details.
  llvm::Instruction *Instruction_;
  unique_ptr<z3::expr> InstructionAssumptions_;

  bool hasSupportedType(llvm::Value *);
  bool hasValidOperands(llvm::Instruction &);

  unsigned bits(llvm::Value *);
  z3::expr constExprValue(llvm::ConstantExpr *);
  z3::expr rValue(llvm::Value *);
  z3::expr lValue();
  z3::expr allocationFormula(z3::expr);

public:
  InstructionSemantics(const FunctionContext &, const Fragment &);

  z3::expr preserve(llvm::Instruction &I) {
    Instruction_ = &I;
    return lValue() == rValue(&I);
  }

  z3::expr visit(llvm::Instruction &I);

  // visit methods for every llvm instruction
  z3::expr visitRet(llvm::ReturnInst &I);
  z3::expr visitBr(llvm::BranchInst &I);
  z3::expr visitSwitch(llvm::SwitchInst &I);
  z3::expr visitIndirectBr(llvm::IndirectBrInst &I);
  z3::expr visitInvoke(llvm::InvokeInst &I);
  z3::expr visitResume(llvm::ResumeInst &I);
  z3::expr visitUnreachable(llvm::UnreachableInst &I);

  z3::expr visitAdd(llvm::BinaryOperator &I);
  z3::expr visitFAdd(llvm::BinaryOperator &I);
  z3::expr visitSub(llvm::BinaryOperator &I);
  z3::expr visitFSub(llvm::BinaryOperator &I);
  z3::expr visitMul(llvm::BinaryOperator &I);
  z3::expr visitFMul(llvm::BinaryOperator &I);
  z3::expr visitUDiv(llvm::BinaryOperator &I);
  z3::expr visitSDiv(llvm::BinaryOperator &I);
  z3::expr visitFDiv(llvm::BinaryOperator &I);
  z3::expr visitURem(llvm::BinaryOperator &I);
  z3::expr visitSRem(llvm::BinaryOperator &I);
  z3::expr visitFRem(llvm::BinaryOperator &I);

  z3::expr visitShl(llvm::BinaryOperator &I);
  z3::expr visitLShr(llvm::BinaryOperator &I);
  z3::expr visitAShr(llvm::BinaryOperator &I);
  z3::expr visitAnd(llvm::BinaryOperator &I);
  z3::expr visitOr(llvm::BinaryOperator &I);
  z3::expr visitXor(llvm::BinaryOperator &I);

  z3::expr visitAlloca(llvm::AllocaInst &I);
  z3::expr visitLoad(llvm::LoadInst &I);
  z3::expr visitStore(llvm::StoreInst &I);
  z3::expr visitGetElementPtr(llvm::GetElementPtrInst &I);
  z3::expr visitFence(llvm::FenceInst &I);
  z3::expr visitAtomicCmpXchg(llvm::AtomicCmpXchgInst &I);
  z3::expr visitAtomicRMW(llvm::AtomicRMWInst &I);

  z3::expr visitTrunc(llvm::TruncInst &I);
  z3::expr visitZExt(llvm::ZExtInst &I);
  z3::expr visitSExt(llvm::SExtInst &I);
  z3::expr visitFPToUI(llvm::FPToUIInst &I);
  z3::expr visitFPToSI(llvm::FPToSIInst &I);
  z3::expr visitUIToFP(llvm::UIToFPInst &I);
  z3::expr visitSIToFP(llvm::SIToFPInst &I);
  z3::expr visitFPTrunc(llvm::FPTruncInst &I);
  z3::expr visitFPExt(llvm::FPExtInst &I);
  z3::expr visitPtrToInt(llvm::PtrToIntInst &I);
  z3::expr visitIntToPtr(llvm::IntToPtrInst &I);
  z3::expr visitBitCast(llvm::BitCastInst &I);
  z3::expr visitAddrSpaceCast(llvm::AddrSpaceCastInst &I);

  z3::expr visitICmp(llvm::ICmpInst &I);
  z3::expr visitFCmp(llvm::FCmpInst &I);
  z3::expr visitPHI(llvm::PHINode &I);
  z3::expr visitCall(llvm::CallInst &I);
  z3::expr visitCallInst(llvm::CallInst &I) { return visitCall(I); }
  z3::expr visitSelect(llvm::SelectInst &I);
  z3::expr visitUserOp1(llvm::Instruction &I) { return preserve(I); }
  z3::expr visitUserOp2(llvm::Instruction &I) { return preserve(I); }
  z3::expr visitVAArg(llvm::VAArgInst &I);
  z3::expr visitExtractElement(llvm::ExtractElementInst &I);
  z3::expr visitInsertElement(llvm::InsertElementInst &I);
  z3::expr visitShuffleVector(llvm::ShuffleVectorInst &I);
  z3::expr visitExtractValue(llvm::ExtractValueInst &I);
  z3::expr visitInsertValue(llvm::InsertValueInst &I);
  z3::expr visitLandingPad(llvm::LandingPadInst &I);

  // LLVM 12/14 support cleanup and catch instructions
  z3::expr visitCleanupReturnInst(llvm::CleanupReturnInst &I);
  z3::expr visitCatchReturnInst(llvm::CatchReturnInst &I);
  z3::expr visitCatchSwitchInst(llvm::CatchSwitchInst &I);
  z3::expr visitFuncletPadInst(llvm::FuncletPadInst &I);
  z3::expr visitCleanupPadInst(llvm::CleanupPadInst &I);
  z3::expr visitCatchPadInst(llvm::CatchPadInst &I);

  // LLVM 12/14 has FreezeInst and FNeg
  z3::expr visitFreezeInst(llvm::FreezeInst &I);
  z3::expr visitFNeg(llvm::UnaryOperator &I);

  // LLVM 12/14 has CallBase and CallBrInst
  z3::expr visitCallBase(llvm::CallBase &CB) { return preserve(CB); }
  z3::expr visitCallBrInst(llvm::CallBrInst &I) { return preserve(I); }
};

} // namespace symabs_ai

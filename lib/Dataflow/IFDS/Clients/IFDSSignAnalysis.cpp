#include "Dataflow/IFDS/Clients/IFDSSignAnalysis.h"

#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/MathExtras.h>

namespace ifds {

llvm::Optional<int64_t> SignAnalysis::as_const_i64(const llvm::Value *v) {
  if (const auto *ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v)) {
    return ci->getSExtValue();
  }
  return llvm::None;
}

SignFact::Sign SignAnalysis::sign_of(int64_t v) {
  if (v < 0) {
    return SignFact::Sign::Negative;
  }
  if (v > 0) {
    return SignFact::Sign::Positive;
  }
  return SignFact::Sign::Zero;
}

llvm::Optional<SignFact::Sign> SignAnalysis::eval_binary(unsigned opcode,
                                                         int64_t a, int64_t b) {
  int64_t out = 0;
  switch (opcode) {
  case llvm::Instruction::Add:
    if (llvm::AddOverflow(a, b, out)) {
      return llvm::None;
    }
    return sign_of(out);
  case llvm::Instruction::Sub:
    if (llvm::SubOverflow(a, b, out)) {
      return llvm::None;
    }
    return sign_of(out);
  case llvm::Instruction::Mul:
    if (llvm::MulOverflow(a, b, out)) {
      return llvm::None;
    }
    return sign_of(out);
  case llvm::Instruction::SDiv:
    if (b == 0 || (a == std::numeric_limits<int64_t>::min() && b == -1)) {
      return llvm::None;
    }
    return sign_of(a / b);
  default:
    return llvm::None;
  }
}

SignAnalysis::FactSet SignAnalysis::normal_flow(const llvm::Instruction *stmt,
                                                const llvm::Instruction *succ,
                                                const SignFact &fact) {
  FactSet result;
  if (!stmt) {
    return result;
  }

  result.insert(fact);

  if (stmt->getType()->isVoidTy()) {
    return result;
  }

  const llvm::Value *def = stmt;

  if (const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(stmt)) {
    result.insert(SignFact::value_sign(def, sign_of(ci->getSExtValue())));
    return result;
  }

  if (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
    auto c0 = as_const_i64(bin->getOperand(0));
    auto c1 = as_const_i64(bin->getOperand(1));
    if (c0 && c1) {
      if (auto sign = eval_binary(bin->getOpcode(), *c0, *c1)) {
        result.insert(SignFact::value_sign(def, *sign));
      } else {
        result.insert(SignFact::value_sign(def, SignFact::Sign::Unknown));
      }
      return result;
    }
  }

  // Propagate sign through direct def-use edges.
  if (fact.kind == SignFact::Kind::ValueSign) {
    for (const auto &operand : stmt->operands()) {
      if (operand.get() == fact.value) {
        result.insert(SignFact::value_sign(def, fact.sign));
        break;
      }
    }
  }

  return result;
}

SignAnalysis::FactSet SignAnalysis::call_flow(const llvm::CallBase *call,
                                              const llvm::Function *callee,
                                              const SignFact &fact) {
  FactSet result;
  if (!call) {
    return result;
  }

  result.insert(fact);

  if (!callee || callee->isDeclaration() ||
      fact.kind != SignFact::Kind::ValueSign) {
    return result;
  }

  unsigned idx = 0;
  for (const llvm::Argument &formal : callee->args()) {
    if (idx >= call->arg_size()) {
      break;
    }
    if (call->getArgOperand(idx) == fact.value) {
      result.insert(SignFact::value_sign(&formal, fact.sign));
    }
    ++idx;
  }

  return result;
}

SignAnalysis::FactSet SignAnalysis::return_flow(const llvm::CallBase *call,
                                                const llvm::Instruction *exit_inst,
                                                const llvm::Instruction *return_site, const llvm::Function *callee,
                                                const SignFact &exit_fact,
                                                const SignFact &call_fact) {
  FactSet result;
  if (!call) {
    return result;
  }

  result.insert(call_fact);

  if (!callee || callee->isDeclaration() ||
      exit_fact.kind != SignFact::Kind::ValueSign) {
    return result;
  }

  if (!call->getType()->isVoidTy()) {
    for (const llvm::BasicBlock &bb : *callee) {
      if (const auto *ret =
              llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator())) {
        if (ret->getReturnValue() == exit_fact.value) {
          result.insert(SignFact::value_sign(call, exit_fact.sign));
          break;
        }
      }
    }
  }

  return result;
}

SignAnalysis::FactSet
SignAnalysis::call_to_return_flow(const llvm::CallBase * /*call*/,
                                  const llvm::Instruction *return_site,
                                  llvm::ArrayRef<const llvm::Function *>
                                      callees,
                                  const SignFact &fact) {
  (void)return_site;
  (void)callees;
  FactSet result;
  result.insert(fact);
  return result;
}

SignAnalysis::FactSet SignAnalysis::initial_facts(const llvm::Function *main) {
  FactSet result;
  result.insert(zero_fact());
  if (!main) {
    return result;
  }

  for (const llvm::Argument &arg : main->args()) {
    result.insert(SignFact::value_sign(&arg, SignFact::Sign::Unknown));
  }
  return result;
}

} // namespace ifds

/*
 *
 * Author: rainoftime
 */
#include "Dataflow/IFDS/Clients/IDEConstantPropagation.h"

#include <limits>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/MathExtras.h>

namespace ifds {

IDEConstantPropagation::FactSet
IDEConstantPropagation::initial_facts(const llvm::Function *main) {
  FactSet seeds;
  for (const llvm::Argument &arg : main->args()) {
    seeds.insert(&arg);
  }
  return seeds;
}

IDEConstantPropagation::Value
IDEConstantPropagation::join(const Value &v1, const Value &v2) const {
  if (v1.kind == LCPValue::Bottom)
    return v2;
  if (v2.kind == LCPValue::Bottom)
    return v1;
  if (v1.kind == LCPValue::Top || v2.kind == LCPValue::Top)
    return Value::top();
  return (v1.value == v2.value) ? v1 : Value::top();
}

bool IDEConstantPropagation::definesValue(const llvm::Instruction *I) {
  return !I->getType()->isVoidTy();
}

const llvm::Value *
IDEConstantPropagation::getDefinedValue(const llvm::Instruction *I) {
  return definesValue(I) ? static_cast<const llvm::Value *>(I) : nullptr;
}

bool IDEConstantPropagation::isCopy(const llvm::Instruction *I,
                                    const llvm::Value *&from) {
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(I)) {
    from = store->getValueOperand();
    return true;
  }
  if (auto *bitcastInst = llvm::dyn_cast<llvm::BitCastInst>(I)) {
    from = bitcastInst->getOperand(0);
    return true;
  }
  if (auto *move = llvm::dyn_cast<llvm::UnaryOperator>(I)) {
    if (move->getOpcode() == llvm::Instruction::FNeg)
      return false;
    from = move->getOperand(0);
    return true;
  }
  return false;
}

llvm::Optional<long long>
IDEConstantPropagation::asConst(const llvm::Value *v) {
  if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    return static_cast<long long>(ci->getSExtValue());
  }
  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(v)) {
    const llvm::Value *ptrOp = load->getPointerOperand()->stripPointerCasts();
    const auto *allocaInst = llvm::dyn_cast<llvm::AllocaInst>(ptrOp);
    if (!allocaInst) {
      return llvm::None;
    }

    const llvm::BasicBlock *parent = load->getParent();
    if (!parent) {
      return llvm::None;
    }

    auto it = load->getIterator();
    while (it != parent->begin()) {
      --it;
      const llvm::Instruction &inst = *it;
      auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst);
      if (!store) {
        continue;
      }

      const llvm::Value *storePtr =
          store->getPointerOperand()->stripPointerCasts();
      if (storePtr != allocaInst) {
        continue;
      }

      return asConst(store->getValueOperand());
    }
  }
  return llvm::None;
}

llvm::Optional<long long>
IDEConstantPropagation::applyBinOp(unsigned opcode, long long a, long long b) {
  long long res = 0;
  switch (opcode) {
  case llvm::Instruction::Add:
    if (llvm::AddOverflow(a, b, res))
      return llvm::None;
    return res;
  case llvm::Instruction::Sub:
    if (llvm::SubOverflow(a, b, res))
      return llvm::None;
    return res;
  case llvm::Instruction::Mul:
    if (llvm::MulOverflow(a, b, res))
      return llvm::None;
    return res;
  case llvm::Instruction::SDiv:
    if (b == 0)
      return llvm::None;
    if (a == std::numeric_limits<long long>::min() && b == -1)
      return llvm::None;
    return a / b;
  case llvm::Instruction::UDiv:
    if (b == 0)
      return llvm::None;
    return static_cast<long long>(static_cast<unsigned long long>(a) /
                                  static_cast<unsigned long long>(b));
  case llvm::Instruction::SRem:
    if (b == 0)
      return llvm::None;
    return a % b;
  case llvm::Instruction::URem:
    if (b == 0)
      return llvm::None;
    return static_cast<long long>(static_cast<unsigned long long>(a) %
                                  static_cast<unsigned long long>(b));
  case llvm::Instruction::And:
    return a & b;
  case llvm::Instruction::Or:
    return a | b;
  case llvm::Instruction::Xor:
    return a ^ b;
  default:
    return llvm::None;
  }
}

IDEConstantPropagation::FactSet
IDEConstantPropagation::normal_flow(const llvm::Instruction *stmt,
                                    const llvm::Instruction *succ,
                                    const Fact &fact) {
  FactSet out;
  // propagate same fact by default
  if (fact)
    out.insert(fact);
  if (!fact) {
    out.insert(fact);
  }

  if (auto *allocaInst = llvm::dyn_cast<llvm::AllocaInst>(stmt)) {
    const llvm::Type *allocated = allocaInst->getAllocatedType();
    if (allocated && allocated->isIntegerTy()) {
      out.insert(allocaInst);
    }
  }

  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const llvm::Value *valueOp = store->getValueOperand();
    const llvm::Value *ptrOp = store->getPointerOperand();
    if (fact == valueOp || !fact) {
      out.insert(ptrOp);
    }
  }

  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    const llvm::Value *ptrOp = load->getPointerOperand();
    if (fact == ptrOp) {
      out.insert(load);
    }
  }

  if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
    const llvm::Value *ptrOp = gep->getPointerOperand();
    if (fact == ptrOp) {
      out.insert(gep);
    }
  }

  if (const llvm::Value *def = getDefinedValue(stmt)) {
    if (auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
      const llvm::Value *op0 = bin->getOperand(0);
      const llvm::Value *op1 = bin->getOperand(1);
      if (!fact && asConst(op0).hasValue() && asConst(op1).hasValue()) {
        out.insert(def);
      }
      if (fact == op0 || fact == op1) {
        out.insert(def);
      }
    } else {
      const llvm::Value *from = nullptr;
      if (isCopy(stmt, from) && fact == from) {
        out.insert(def);
      }
    }
  }
  return out;
}

IDEConstantPropagation::FactSet
IDEConstantPropagation::call_flow(const llvm::CallBase *call,
                                  const llvm::Function *callee,
                                  const Fact &fact) {
  FactSet out;
  if (!callee || callee->isDeclaration())
    return out;
  // map actual to formal by position
  for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
    if (fact == call->getArgOperand(i)) {
      const auto *it = callee->arg_begin();
      std::advance(it, i);
      out.insert(&*it);
    }
    if (!fact) {
      if (auto *ci =
              llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(i))) {
        (void)ci;
        const auto *it = callee->arg_begin();
        std::advance(it, i);
        out.insert(&*it);
      }
    }
  }
  return out;
}

IDEConstantPropagation::FactSet IDEConstantPropagation::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const Fact &exit_fact, const Fact &call_fact) {
  (void)exit_inst;
  FactSet out;
  if (!callee || callee->isDeclaration())
    return out;
  // Propagate caller facts unchanged (local variables survive the call).
  if (call_fact)
    out.insert(call_fact);
  // Map the callee's return value back to the call-site result.
  // The old code unconditionally inserted the call instruction regardless of
  // exit_fact, which caused the call result to appear as a fact even when the
  // callee returned nothing relevant.  We now only add the call result when
  // exit_fact is a ReturnInst whose return value is non-null (i.e. the callee
  // actually returns a value on this path).
  if (!call->getType()->isVoidTy()) {
    if (const auto *ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(exit_fact)) {
      if (ret->getReturnValue()) {
        out.insert(static_cast<const llvm::Value *>(call));
      }
    }
  }
  return out;
}

IDEConstantPropagation::FactSet
IDEConstantPropagation::call_to_return_flow(const llvm::CallBase *call,
                                            const llvm::Instruction *return_site,
                                            llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  // conservative: propagate caller facts across unknown call
  if (fact)
    out.insert(fact);
  if (!call->getType()->isVoidTy()) {
    out.insert(static_cast<const llvm::Value *>(call));
  }
  return out;
}

IDEConstantPropagation::EdgeFunction
IDEConstantPropagation::normal_edge_function(const llvm::Instruction *stmt,
                                             const llvm::Instruction *succ,
                                             const Fact &src_fact,
                                             const Fact &tgt_fact) {
  if (auto *allocaInst = llvm::dyn_cast<llvm::AllocaInst>(stmt)) {
    if (tgt_fact == allocaInst && !src_fact) {
      // A freshly-allocated stack slot is "undefined" until written.
      return [](const Value & /*v*/) { return LCPValue::bottom(); };
    }
  }

  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const llvm::Value *valueOp = store->getValueOperand();
    const llvm::Value *ptrOp = store->getPointerOperand();
    if (tgt_fact == ptrOp) {
      if (!src_fact) {
        auto c = asConst(valueOp);
        if (c.hasValue()) {
          long long k = c.getValue();
          return [k](const Value & /*v*/) { return Value::constant(k); };
        }
        return [](const Value & /*v*/) { return LCPValue::top(); };
      }
      if (src_fact == valueOp) {
        return [](const Value &v) { return v; };
      }
      return [](const Value & /*v*/) { return LCPValue::bottom(); };
    }
  }

  if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    const llvm::Value *ptrOp = load->getPointerOperand();
    if (tgt_fact == load && src_fact == ptrOp) {
      return [](const Value &v) { return v; };
    }
  }

  if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
    const llvm::Value *ptrOp = gep->getPointerOperand();
    if (tgt_fact == gep && src_fact == ptrOp) {
      return [](const Value &v) { return v; };
    }
  }

  // If this instruction defines tgt_fact via constant or binop, compute value
  if (const llvm::Value *def = getDefinedValue(stmt)) {
    if (tgt_fact == def) {
      // copy
      const llvm::Value *from = nullptr;
      if (isCopy(stmt, from) && from == src_fact) {
        return [](const Value &v) { return v; };
      }
      // binary op
      if (auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
        const llvm::Value *op0 = bin->getOperand(0);
        const llvm::Value *op1 = bin->getOperand(1);
        unsigned opc = bin->getOpcode();
        return [opc, op0, op1, src_fact](const Value &v) {
          auto c0 = asConst(op0);
          auto c1 = asConst(op1);
          if (!src_fact && c0.hasValue() && c1.hasValue()) {
            auto r = applyBinOp(opc, c0.getValue(), c1.getValue());
            return r.hasValue() ? LCPValue::constant(r.getValue())
                                : LCPValue::top();
          }
          if (src_fact == op0 && c1.hasValue()) {
            if (v.kind == LCPValue::Const) {
              auto r = applyBinOp(opc, v.value, c1.getValue());
              return r.hasValue() ? LCPValue::constant(r.getValue())
                                  : LCPValue::top();
            }
            return v;
          }
          if (src_fact == op1 && c0.hasValue()) {
            if (v.kind == LCPValue::Const) {
              auto r = applyBinOp(opc, c0.getValue(), v.value);
              return r.hasValue() ? LCPValue::constant(r.getValue())
                                  : LCPValue::top();
            }
            return v;
          }
          if (c0.hasValue() && c1.hasValue()) {
            auto r = applyBinOp(opc, c0.getValue(), c1.getValue());
            return r.hasValue() ? LCPValue::constant(r.getValue())
                                : LCPValue::top();
          }
          return LCPValue::bottom();
        };
      }
      // otherwise, unknown definition => top
      return [](const Value & /*v*/) { return LCPValue::top(); };
    }
  }
  // default: identity
  return [](const Value &v) { return v; };
}

IDEConstantPropagation::EdgeFunction IDEConstantPropagation::call_edge_function(
    const llvm::CallBase *call, const llvm::Function *callee,
    const Fact &src_fact, const Fact &tgt_fact) {
  // actual to formal: identity on carried constant, constant propagation for
  // literal args
  if (callee) {
    unsigned idx = 0;
    for (const llvm::Argument &arg : callee->args()) {
      if (&arg == tgt_fact && idx < call->arg_size()) {
        const llvm::Value *actual = call->getArgOperand(idx);
        if (!src_fact) {
          auto c = asConst(actual);
          if (c.hasValue()) {
            long long k = c.getValue();
            return [k](const Value & /*v*/) { return Value::constant(k); };
          }
        }
        if (src_fact == actual) {
          return [](const Value &v) { return v; };
        }
        return [](const Value & /*v*/) { return LCPValue::bottom(); };
      }
      ++idx;
    }
  }
  return [](const Value &v) { return v; };
}

IDEConstantPropagation::EdgeFunction
IDEConstantPropagation::return_edge_function(const llvm::CallBase *call,
                                             const llvm::Function *callee,
                                             const llvm::Instruction *exit_inst,
                                             const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                             const Fact &ret_fact) {
  // callee return to caller result
  if (!call->getType()->isVoidTy()) {
    const llvm::Value *callDef = static_cast<const llvm::Value *>(call);
    if (ret_fact == callDef) {
      // propagate value from exit_fact (assumed return value) directly
      return [](const Value &v) { return v; };
    }
  }
  return [](const Value &v) { return v; };
}

IDEConstantPropagation::EdgeFunction
IDEConstantPropagation::call_to_return_edge_function(const llvm::CallBase *call,
                                                     const llvm::Instruction *return_site,
                                                     llvm::ArrayRef<const llvm::Function *> callees,
                                                     const Fact &src_fact,
                                                     const Fact &tgt_fact) {
  (void)return_site;
  (void)callees;
  // unknown function: kill definition of call result => top; keep others
  if (!call->getType()->isVoidTy()) {
    const llvm::Value *callDef = static_cast<const llvm::Value *>(call);
    if (tgt_fact == callDef) {
      return [](const Value & /*v*/) { return LCPValue::top(); };
    }
  }
  (void)src_fact;
  (void)tgt_fact;
  return [](const Value &v) { return v; };
}

IDEConstantPropagation::FactSet
IDEConstantPropagation::summary_flow(const llvm::CallBase *call,
                                     const llvm::Function *callee,
                                     const Fact &fact) {
  FactSet out;
  if (!callee || !callee->isIntrinsic()) {
    return out;
  }
  if (!call->getType()->isIntegerTy()) {
    return out;
  }
  if (call->arg_size() != 2) {
    return out;
  }

  const llvm::Value *op0 = call->getArgOperand(0);
  const llvm::Value *op1 = call->getArgOperand(1);
  if (!fact || fact == op0 || fact == op1) {
    out.insert(static_cast<const llvm::Value *>(call));
  }
  return out;
}

IDEConstantPropagation::EdgeFunction
IDEConstantPropagation::summary_edge_function(const llvm::CallBase *call,
                                              const llvm::Function *callee,
                                              const llvm::Instruction *return_site,
                                              const Fact &src_fact,
                                              const Fact &tgt_fact) {
  (void)return_site;
  if (!callee || !callee->isIntrinsic()) {
    return [](const Value &v) { return v; };
  }
  if (tgt_fact != static_cast<const llvm::Value *>(call)) {
    return [](const Value &v) { return v; };
  }

  auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
  if (!intrinsic || intrinsic->arg_size() != 2) {
    return [](const Value &v) { return v; };
  }

  unsigned opc = 0;
  switch (intrinsic->getIntrinsicID()) {
  case llvm::Intrinsic::sadd_sat:
  case llvm::Intrinsic::uadd_sat:
    opc = llvm::Instruction::Add;
    break;
  case llvm::Intrinsic::ssub_sat:
  case llvm::Intrinsic::usub_sat:
    opc = llvm::Instruction::Sub;
    break;
  case llvm::Intrinsic::smul_fix:
  case llvm::Intrinsic::smul_fix_sat:
  case llvm::Intrinsic::umul_fix:
  case llvm::Intrinsic::umul_fix_sat:
    opc = llvm::Instruction::Mul;
    break;
  default:
    return [](const Value &v) { return v; };
  }

  const llvm::Value *op0 = intrinsic->getArgOperand(0);
  const llvm::Value *op1 = intrinsic->getArgOperand(1);

  return [opc, op0, op1, src_fact](const Value &v) {
    auto c0 = asConst(op0);
    auto c1 = asConst(op1);
    if (!src_fact && c0.hasValue() && c1.hasValue()) {
      auto r = applyBinOp(opc, c0.getValue(), c1.getValue());
      return r.hasValue() ? LCPValue::constant(r.getValue()) : LCPValue::top();
    }
    if (src_fact == op0 && c1.hasValue()) {
      if (v.kind == LCPValue::Const) {
        auto r = applyBinOp(opc, v.value, c1.getValue());
        return r.hasValue() ? LCPValue::constant(r.getValue())
                            : LCPValue::top();
      }
      return v;
    }
    if (src_fact == op1 && c0.hasValue()) {
      if (v.kind == LCPValue::Const) {
        auto r = applyBinOp(opc, c0.getValue(), v.value);
        return r.hasValue() ? LCPValue::constant(r.getValue())
                            : LCPValue::top();
      }
      return v;
    }
    if (c0.hasValue() && c1.hasValue()) {
      auto r = applyBinOp(opc, c0.getValue(), c1.getValue());
      return r.hasValue() ? LCPValue::constant(r.getValue()) : LCPValue::top();
    }
    return LCPValue::bottom();
  };
}

} // namespace ifds

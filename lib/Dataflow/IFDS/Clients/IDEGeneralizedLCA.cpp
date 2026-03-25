#include "Dataflow/IFDS/Clients/IDEGeneralizedLCA.h"

#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/MathExtras.h>

namespace ifds {

llvm::Optional<int64_t> IDEGeneralizedLCA::as_const(const llvm::Value *v) {
  if (const auto *ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v)) {
    return ci->getSExtValue();
  }
  return llvm::None;
}

llvm::Optional<int64_t> IDEGeneralizedLCA::apply_binop(unsigned opcode,
                                                       int64_t a, int64_t b) {
  int64_t out = 0;
  switch (opcode) {
  case llvm::Instruction::Add:
    if (llvm::AddOverflow(a, b, out)) {
      return llvm::None;
    }
    return out;
  case llvm::Instruction::Sub:
    if (llvm::SubOverflow(a, b, out)) {
      return llvm::None;
    }
    return out;
  case llvm::Instruction::Mul:
    if (llvm::MulOverflow(a, b, out)) {
      return llvm::None;
    }
    return out;
  case llvm::Instruction::SDiv:
    if (b == 0 || (a == std::numeric_limits<int64_t>::min() && b == -1)) {
      return llvm::None;
    }
    return a / b;
  default:
    return llvm::None;
  }
}

IDEGeneralizedLCA::Value
IDEGeneralizedLCA::cap_constants(std::set<int64_t> values) {
  if (values.size() > kMaxSetSize) {
    return Value::top();
  }
  return Value(std::move(values));
}

IDEGeneralizedLCA::FactSet
IDEGeneralizedLCA::normal_flow(const llvm::Instruction *stmt,
                               const llvm::Instruction *succ,
                               const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (!stmt || stmt->getType()->isVoidTy()) {
    return out;
  }
  if (fact == zero_fact()) {
    out.insert(stmt);
    return out;
  }
  for (const auto &op : stmt->operands()) {
    if (op.get() == fact) {
      out.insert(stmt);
      break;
    }
  }
  return out;
}

IDEGeneralizedLCA::FactSet
IDEGeneralizedLCA::call_flow(const llvm::CallBase *call,
                             const llvm::Function *callee, const Fact &fact) {
  FactSet out;
  if (fact == zero_fact()) {
    out.insert(fact);
  }
  if (!call || !callee || callee->isDeclaration()) {
    return out;
  }
  for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
    if (fact == call->getArgOperand(i)) {
      const auto *it = callee->arg_begin();
      std::advance(it, i);
      out.insert(&*it);
    }
  }
  return out;
}

IDEGeneralizedLCA::FactSet
IDEGeneralizedLCA::return_flow(const llvm::CallBase *call,
                               const llvm::Instruction *exit_inst,
                               const llvm::Instruction *return_site, const llvm::Function *callee,
                               const Fact &exit_fact, const Fact &call_fact) {
  FactSet out;
  if (!call) {
    return out;
  }
  if (call_fact != zero_fact()) {
    out.insert(call_fact);
  }
  if (!callee || callee->isDeclaration()) {
    return out;
  }
  if (!call->getType()->isVoidTy()) {
    for (const llvm::BasicBlock &bb : *callee) {
      if (const auto *ret =
              llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator())) {
        if (ret->getReturnValue() == exit_fact) {
          out.insert(call);
          break;
        }
      }
    }
  }
  return out;
}

IDEGeneralizedLCA::FactSet
IDEGeneralizedLCA::call_to_return_flow(const llvm::CallBase *call,
                                       const llvm::Instruction *return_site,
                                       llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (call && !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  return out;
}

IDEGeneralizedLCA::FactSet
IDEGeneralizedLCA::initial_facts(const llvm::Function *main) {
  FactSet out;
  out.insert(zero_fact());
  if (!main) {
    return out;
  }
  for (const llvm::Argument &arg : main->args()) {
    out.insert(&arg);
  }
  return out;
}

IDEGeneralizedLCA::Value IDEGeneralizedLCA::join(const Value &v1,
                                                 const Value &v2) const {
  if (v1.kind == Value::Bottom) {
    return v2;
  }
  if (v2.kind == Value::Bottom) {
    return v1;
  }
  if (v1.kind == Value::Top || v2.kind == Value::Top) {
    return Value::top();
  }
  std::set<int64_t> merged = v1.constants;
  merged.insert(v2.constants.begin(), v2.constants.end());
  return cap_constants(std::move(merged));
}

IDEGeneralizedLCA::EdgeFunction IDEGeneralizedLCA::normal_edge_function(
    const llvm::Instruction *stmt, const llvm::Instruction *succ,
    const Fact &src_fact, const Fact &tgt_fact) {
  (void)succ;
  if (!stmt) {
    return [](const Value &v) { return v; };
  }

  if (src_fact == tgt_fact) {
    return [](const Value &v) { return v; };
  }

  if (tgt_fact == stmt) {
    if (const auto *const_int = llvm::dyn_cast<llvm::ConstantInt>(stmt)) {
      const int64_t c = const_int->getSExtValue();
      return [c](const Value & /*v*/) { return Value::singleton(c); };
    }

    if (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
      const llvm::Value *op0 = bin->getOperand(0);
      const llvm::Value *op1 = bin->getOperand(1);
      const unsigned opcode = bin->getOpcode();
      auto c0 = as_const(op0);
      auto c1 = as_const(op1);

      if (src_fact == zero_fact() && c0.hasValue() && c1.hasValue()) {
        auto r = apply_binop(opcode, c0.getValue(), c1.getValue());
        if (r.hasValue()) {
          const int64_t c = r.getValue();
          return [c](const Value & /*v*/) { return Value::singleton(c); };
        }
        return [](const Value & /*v*/) { return Value::top(); };
      }

      if (src_fact == op0 && c1.hasValue()) {
        const int64_t k = c1.getValue();
        return [opcode, k](const Value &v) {
          if (v.kind == Value::Top || v.kind == Value::Bottom) {
            return v;
          }
          std::set<int64_t> out;
          for (int64_t c : v.constants) {
            auto r = apply_binop(opcode, c, k);
            if (!r.hasValue()) {
              return Value::top();
            }
            out.insert(r.getValue());
          }
          return cap_constants(std::move(out));
        };
      }

      if (src_fact == op1 && c0.hasValue()) {
        const int64_t k = c0.getValue();
        return [opcode, k](const Value &v) {
          if (v.kind == Value::Top || v.kind == Value::Bottom) {
            return v;
          }
          std::set<int64_t> out;
          for (int64_t c : v.constants) {
            auto r = apply_binop(opcode, k, c);
            if (!r.hasValue()) {
              return Value::top();
            }
            out.insert(r.getValue());
          }
          return cap_constants(std::move(out));
        };
      }
    }
  }

  return [](const Value &v) { return v; };
}

IDEGeneralizedLCA::EdgeFunction
IDEGeneralizedLCA::call_edge_function(const llvm::CallBase * /*call*/,
                                      const llvm::Function * /*callee*/,
                                      const Fact & /*src_fact*/,
                                      const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDEGeneralizedLCA::EdgeFunction
IDEGeneralizedLCA::return_edge_function(const llvm::CallBase * /*call*/,
                                        const llvm::Function * /*callee*/,
                                        const llvm::Instruction * /*exit_inst*/,
                                        const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                        const Fact & /*ret_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDEGeneralizedLCA::EdgeFunction
IDEGeneralizedLCA::call_to_return_edge_function(const llvm::CallBase * /*call*/,
                                                const llvm::Instruction *return_site,
                                                llvm::ArrayRef<const llvm::Function *> /*callees*/,
                                                const Fact & /*src_fact*/,
                                                const Fact & /*tgt_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

} // namespace ifds

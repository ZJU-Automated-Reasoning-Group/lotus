#include "Dataflow/IFDS/Clients/IDESecureHeapPropagation.h"

#include <llvm/IR/Instructions.h>

namespace ifds {

IDESecureHeapPropagation::IDESecureHeapPropagation() {
  m_allocators = {"malloc", "calloc", "realloc", "_Znwm", "_Znam"};
  m_releasers = {"free", "_ZdlPv", "_ZdaPv"};
  m_securers = {"memset_s", "explicit_bzero", "OPENSSL_cleanse"};
}

IDESecureHeapPropagation::FactSet
IDESecureHeapPropagation::normal_flow(const llvm::Instruction *stmt,
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

IDESecureHeapPropagation::FactSet
IDESecureHeapPropagation::call_flow(const llvm::CallBase *call,
                                    const llvm::Function *callee,
                                    const Fact &fact) {
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

IDESecureHeapPropagation::FactSet IDESecureHeapPropagation::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const Fact &exit_fact, const Fact &call_fact) {
  (void)exit_inst;
  (void)return_site;
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

IDESecureHeapPropagation::FactSet
IDESecureHeapPropagation::call_to_return_flow(const llvm::CallBase *call,
                                              const llvm::Instruction *return_site,
                                              llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (call && !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  return out;
}

IDESecureHeapPropagation::FactSet
IDESecureHeapPropagation::initial_facts(const llvm::Function *main) {
  FactSet out;
  out.insert(zero_fact());
  if (!main) {
    return out;
  }
  for (const llvm::Argument &arg : main->args()) {
    if (arg.getType()->isPointerTy()) {
      out.insert(&arg);
    }
  }
  return out;
}

IDESecureHeapPropagation::Value
IDESecureHeapPropagation::join(const Value &v1, const Value &v2) const {
  if (v1.kind == Value::Bottom) {
    return v2;
  }
  if (v2.kind == Value::Bottom) {
    return v1;
  }
  if (v1.kind == v2.kind) {
    return v1;
  }
  if (v1.kind == Value::Error || v2.kind == Value::Error) {
    return Value::error();
  }
  if (v1.kind == Value::Top || v2.kind == Value::Top) {
    return Value::top();
  }
  return Value::top();
}

IDESecureHeapPropagation::EdgeFunction
IDESecureHeapPropagation::normal_edge_function(
    const llvm::Instruction * /*stmt*/, const llvm::Instruction * /*succ*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDESecureHeapPropagation::EdgeFunction
IDESecureHeapPropagation::call_edge_function(const llvm::CallBase * /*call*/,
                                             const llvm::Function * /*callee*/,
                                             const Fact & /*src_fact*/,
                                             const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDESecureHeapPropagation::EdgeFunction
IDESecureHeapPropagation::return_edge_function(const llvm::CallBase * /*call*/,
                                               const llvm::Function * /*callee*/,
                                               const llvm::Instruction * /*exit_inst*/,
                                               const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                               const Fact & /*ret_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDESecureHeapPropagation::EdgeFunction
IDESecureHeapPropagation::call_to_return_edge_function(
    const llvm::CallBase * /*call*/, const llvm::Instruction *return_site,
    llvm::ArrayRef<const llvm::Function *> /*callees*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDESecureHeapPropagation::FactSet
IDESecureHeapPropagation::summary_flow(const llvm::CallBase *call,
                                       const llvm::Function *callee,
                                       const Fact &fact) {
  FactSet out;
  if (!call || !callee) {
    return out;
  }
  const std::string name = callee->getName().str();
  if (m_allocators.count(name) > 0 && fact == zero_fact() &&
      !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  if ((m_releasers.count(name) > 0 || m_securers.count(name) > 0) &&
      call->arg_size() > 0 && fact == call->getArgOperand(0)) {
    out.insert(fact);
  }
  return out;
}

IDESecureHeapPropagation::EdgeFunction
IDESecureHeapPropagation::summary_edge_function(const llvm::CallBase *call,
                                                const llvm::Function *callee,
                                                const llvm::Instruction *return_site,
                                                const Fact & /*src_fact*/,
                                                const Fact & /*tgt_fact*/) {
  (void)call;
  (void)return_site;
  if (!callee) {
    return [](const Value &v) { return v; };
  }
  const std::string name = callee->getName().str();
  if (m_allocators.count(name) > 0) {
    return [](const Value & /*v*/) { return Value::allocated(); };
  }
  if (m_securers.count(name) > 0) {
    return [](const Value &v) {
      if (v.kind == Value::Freed) {
        return Value::error();
      }
      return Value::secured();
    };
  }
  if (m_releasers.count(name) > 0) {
    return [](const Value &v) {
      if (v.kind == Value::Freed) {
        return Value::error();
      }
      return Value::freed();
    };
  }
  return [](const Value &v) { return v; };
}

} // namespace ifds

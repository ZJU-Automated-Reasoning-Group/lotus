#include "Dataflow/IFDS/Clients/IDEExtendedTaintAnalysis.h"

#include <llvm/IR/Instructions.h>

namespace ifds {

IDEExtendedTaintAnalysis::IDEExtendedTaintAnalysis() {
  m_sources = {"recv", "read", "fgets", "getline", "getenv"};
  m_sanitizers = {"sanitize", "escape", "strncpy_s", "memset_s"};
}

bool IDEExtendedTaintAnalysis::is_source_function(
    const llvm::Function *callee) const {
  return callee && m_sources.count(callee->getName().str()) > 0;
}

bool IDEExtendedTaintAnalysis::is_sanitizer_function(
    const llvm::Function *callee) const {
  return callee && m_sanitizers.count(callee->getName().str()) > 0;
}

IDEExtendedTaintAnalysis::FactSet
IDEExtendedTaintAnalysis::normal_flow(const llvm::Instruction *stmt,
                                      const llvm::Instruction *succ,
                                      const Fact &fact) {
  FactSet out;
  out.insert(fact);

  if (!stmt || stmt->getType()->isVoidTy()) {
    return out;
  }
  const Fact def = stmt;
  if (fact == zero_fact()) {
    out.insert(def);
    return out;
  }
  for (const auto &op : stmt->operands()) {
    if (op.get() == fact) {
      out.insert(def);
      break;
    }
  }
  return out;
}

IDEExtendedTaintAnalysis::FactSet
IDEExtendedTaintAnalysis::call_flow(const llvm::CallBase *call,
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

IDEExtendedTaintAnalysis::FactSet IDEExtendedTaintAnalysis::return_flow(
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

IDEExtendedTaintAnalysis::FactSet
IDEExtendedTaintAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                              const llvm::Instruction *return_site,
                                              llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (call && !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  return out;
}

IDEExtendedTaintAnalysis::FactSet
IDEExtendedTaintAnalysis::initial_facts(const llvm::Function *main) {
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

IDEExtendedTaintAnalysis::Value
IDEExtendedTaintAnalysis::join(const Value &v1, const Value &v2) const {
  if (v1.kind == Value::Bottom) {
    return v2;
  }
  if (v2.kind == Value::Bottom) {
    return v1;
  }
  if (v1.kind == v2.kind) {
    return v1;
  }
  if (v1.kind == Value::Top || v2.kind == Value::Top) {
    return Value::top();
  }
  if (v1.kind == Value::Tainted || v2.kind == Value::Tainted) {
    return Value::tainted();
  }
  return Value::top();
}

IDEExtendedTaintAnalysis::EdgeFunction
IDEExtendedTaintAnalysis::normal_edge_function(
    const llvm::Instruction * /*stmt*/, const llvm::Instruction * /*succ*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDEExtendedTaintAnalysis::EdgeFunction
IDEExtendedTaintAnalysis::call_edge_function(const llvm::CallBase * /*call*/,
                                             const llvm::Function * /*callee*/,
                                             const Fact & /*src_fact*/,
                                             const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDEExtendedTaintAnalysis::EdgeFunction
IDEExtendedTaintAnalysis::return_edge_function(const llvm::CallBase * /*call*/,
                                               const llvm::Function * /*callee*/,
                                               const llvm::Instruction * /*exit_inst*/,
                                               const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                               const Fact & /*ret_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDEExtendedTaintAnalysis::EdgeFunction
IDEExtendedTaintAnalysis::call_to_return_edge_function(
    const llvm::CallBase * /*call*/, const llvm::Instruction *return_site,
    llvm::ArrayRef<const llvm::Function *> /*callees*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDEExtendedTaintAnalysis::FactSet
IDEExtendedTaintAnalysis::summary_flow(const llvm::CallBase *call,
                                       const llvm::Function *callee,
                                       const Fact &fact) {
  FactSet out;
  if (!call || !callee) {
    return out;
  }
  if (is_source_function(callee) && fact == zero_fact() &&
      !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  if (is_sanitizer_function(callee) && fact != zero_fact() &&
      !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  return out;
}

IDEExtendedTaintAnalysis::EdgeFunction
IDEExtendedTaintAnalysis::summary_edge_function(const llvm::CallBase *call,
                                                const llvm::Function *callee,
                                                const llvm::Instruction *return_site,
                                                const Fact & /*src_fact*/,
                                                const Fact & /*tgt_fact*/) {
  (void)call;
  (void)return_site;
  if (is_source_function(callee)) {
    return [](const Value & /*v*/) { return Value::tainted(); };
  }
  if (is_sanitizer_function(callee)) {
    return [](const Value & /*v*/) { return Value::sanitized(); };
  }
  return [](const Value &v) { return v; };
}

} // namespace ifds

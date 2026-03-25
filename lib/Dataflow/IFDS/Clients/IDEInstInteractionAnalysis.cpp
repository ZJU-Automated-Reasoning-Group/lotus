#include "Dataflow/IFDS/Clients/IDEInstInteractionAnalysis.h"

#include <llvm/IR/Instructions.h>

namespace ifds {

IDEInstInteractionAnalysis::FactSet
IDEInstInteractionAnalysis::normal_flow(const llvm::Instruction *stmt,
                                        const llvm::Instruction *succ,
                                        const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (!stmt) {
    return out;
  }

  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    if (fact == load->getPointerOperand()) {
      out.insert(load);
    }
  } else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    if (fact == store->getPointerOperand() ||
        fact == store->getValueOperand()) {
      out.insert(store->getPointerOperand());
    }
  } else if (!stmt->getType()->isVoidTy()) {
    for (const auto &op : stmt->operands()) {
      if (op.get() == fact) {
        out.insert(stmt);
        break;
      }
    }
  }
  return out;
}

IDEInstInteractionAnalysis::FactSet
IDEInstInteractionAnalysis::call_flow(const llvm::CallBase *call,
                                      const llvm::Function *callee,
                                      const Fact &fact) {
  FactSet out;
  if (!call || !callee || callee->isDeclaration()) {
    out.insert(fact);
    return out;
  }
  for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
    if (fact == call->getArgOperand(i)) {
      const auto *it = callee->arg_begin();
      std::advance(it, i);
      out.insert(&*it);
    }
  }
  out.insert(fact);
  return out;
}

IDEInstInteractionAnalysis::FactSet IDEInstInteractionAnalysis::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const Fact &exit_fact, const Fact &call_fact) {
  (void)exit_inst;
  (void)return_site;
  FactSet out;
  if (!call) {
    return out;
  }
  out.insert(call_fact);
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

IDEInstInteractionAnalysis::FactSet
IDEInstInteractionAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                                const llvm::Instruction *return_site,
                                                llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (call && !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  return out;
}

IDEInstInteractionAnalysis::FactSet
IDEInstInteractionAnalysis::initial_facts(const llvm::Function *main) {
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

IDEInstInteractionAnalysis::Value
IDEInstInteractionAnalysis::join(const Value &v1, const Value &v2) const {
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
  if ((v1.kind == Value::Read && v2.kind == Value::Write) ||
      (v1.kind == Value::Write && v2.kind == Value::Read) ||
      v1.kind == Value::ReadWrite || v2.kind == Value::ReadWrite) {
    return Value::read_write();
  }
  if (v1.kind == Value::None) {
    return v2;
  }
  if (v2.kind == Value::None) {
    return v1;
  }
  return Value::top();
}

IDEInstInteractionAnalysis::EdgeFunction
IDEInstInteractionAnalysis::normal_edge_function(const llvm::Instruction *stmt,
                                                 const llvm::Instruction *succ,
                                                 const Fact &src_fact,
                                                 const Fact &tgt_fact) {
  if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
    if (src_fact == load->getPointerOperand() && tgt_fact == load) {
      return [](const Value &v) {
        if (v.kind == Value::Write || v.kind == Value::ReadWrite) {
          return Value::read_write();
        }
        return Value::read();
      };
    }
  }
  if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    if (src_fact == store->getPointerOperand() &&
        tgt_fact == store->getPointerOperand()) {
      return [](const Value &v) {
        if (v.kind == Value::Read || v.kind == Value::ReadWrite) {
          return Value::read_write();
        }
        return Value::write();
      };
    }
  }
  return [](const Value &v) { return v; };
}

IDEInstInteractionAnalysis::EdgeFunction
IDEInstInteractionAnalysis::call_edge_function(const llvm::CallBase * /*call*/,
                                               const llvm::Function * /*callee*/,
                                               const Fact & /*src_fact*/,
                                               const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDEInstInteractionAnalysis::EdgeFunction
IDEInstInteractionAnalysis::return_edge_function(
    const llvm::CallBase * /*call*/, const llvm::Function * /*callee*/,
    const llvm::Instruction * /*exit_inst*/,
    const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
    const Fact & /*ret_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDEInstInteractionAnalysis::EdgeFunction
IDEInstInteractionAnalysis::call_to_return_edge_function(
    const llvm::CallBase * /*call*/, const llvm::Instruction *return_site,
    llvm::ArrayRef<const llvm::Function *> /*callees*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

} // namespace ifds

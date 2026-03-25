#include "Dataflow/IFDS/Clients/IDEFeatureTaintAnalysis.h"

#include <llvm/IR/Instructions.h>

namespace ifds {

IDEFeatureTaintAnalysis::IDEFeatureTaintAnalysis() {
  m_source_feature_bits["recv"] = 1ull << 0;
  m_source_feature_bits["read"] = 1ull << 1;
  m_source_feature_bits["getenv"] = 1ull << 2;
  m_source_feature_bits["fgets"] = 1ull << 3;

  // Bitmask bits to clear for sanitizers.
  m_sanitizer_clears["escape_sql"] = (1ull << 0) | (1ull << 2);
  m_sanitizer_clears["escape_html"] = (1ull << 1) | (1ull << 3);
}

IDEFeatureTaintAnalysis::FactSet
IDEFeatureTaintAnalysis::normal_flow(const llvm::Instruction *stmt,
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

IDEFeatureTaintAnalysis::FactSet
IDEFeatureTaintAnalysis::call_flow(const llvm::CallBase *call,
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

IDEFeatureTaintAnalysis::FactSet IDEFeatureTaintAnalysis::return_flow(
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

IDEFeatureTaintAnalysis::FactSet
IDEFeatureTaintAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                             const llvm::Instruction *return_site,
                                             llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) {
  FactSet out;
  out.insert(fact);
  if (call && !call->getType()->isVoidTy()) {
    out.insert(call);
  }
  return out;
}

IDEFeatureTaintAnalysis::FactSet
IDEFeatureTaintAnalysis::initial_facts(const llvm::Function *main) {
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

IDEFeatureTaintAnalysis::Value
IDEFeatureTaintAnalysis::join(const Value &v1, const Value &v2) const {
  if (v1.kind == Value::Bottom) {
    return v2;
  }
  if (v2.kind == Value::Bottom) {
    return v1;
  }
  if (v1.kind == Value::Top || v2.kind == Value::Top) {
    return Value::top();
  }
  return Value::features(v1.mask | v2.mask);
}

IDEFeatureTaintAnalysis::EdgeFunction
IDEFeatureTaintAnalysis::normal_edge_function(
    const llvm::Instruction * /*stmt*/, const llvm::Instruction * /*succ*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDEFeatureTaintAnalysis::EdgeFunction
IDEFeatureTaintAnalysis::call_edge_function(const llvm::CallBase * /*call*/,
                                            const llvm::Function * /*callee*/,
                                            const Fact & /*src_fact*/,
                                            const Fact & /*tgt_fact*/) {
  return [](const Value &v) { return v; };
}

IDEFeatureTaintAnalysis::EdgeFunction
IDEFeatureTaintAnalysis::return_edge_function(const llvm::CallBase * /*call*/,
                                              const llvm::Function * /*callee*/,
                                              const llvm::Instruction * /*exit_inst*/,
                                              const llvm::Instruction *return_site, const Fact & /*exit_fact*/,
                                              const Fact & /*ret_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDEFeatureTaintAnalysis::EdgeFunction
IDEFeatureTaintAnalysis::call_to_return_edge_function(
    const llvm::CallBase * /*call*/, const llvm::Instruction *return_site,
    llvm::ArrayRef<const llvm::Function *> /*callees*/,
    const Fact & /*src_fact*/,
    const Fact & /*tgt_fact*/) {
  (void)return_site;
  return [](const Value &v) { return v; };
}

IDEFeatureTaintAnalysis::FactSet
IDEFeatureTaintAnalysis::summary_flow(const llvm::CallBase *call,
                                      const llvm::Function *callee,
                                      const Fact &fact) {
  FactSet out;
  if (!call || !callee || call->getType()->isVoidTy()) {
    return out;
  }
  const std::string name = callee->getName().str();
  if (m_source_feature_bits.count(name) > 0 && fact == zero_fact()) {
    out.insert(call);
  }
  if (m_sanitizer_clears.count(name) > 0 && fact != zero_fact()) {
    out.insert(call);
  }
  return out;
}

IDEFeatureTaintAnalysis::EdgeFunction
IDEFeatureTaintAnalysis::summary_edge_function(const llvm::CallBase *call,
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
  auto srcIt = m_source_feature_bits.find(name);
  if (srcIt != m_source_feature_bits.end()) {
    const uint64_t bit = srcIt->second;
    return [bit](const Value &v) {
      if (v.kind == Value::Top) {
        return v;
      }
      const uint64_t base = (v.kind == Value::Features) ? v.mask : 0;
      return Value::features(base | bit);
    };
  }
  auto sanIt = m_sanitizer_clears.find(name);
  if (sanIt != m_sanitizer_clears.end()) {
    const uint64_t clearMask = sanIt->second;
    return [clearMask](const Value &v) {
      if (v.kind == Value::Top || v.kind == Value::Bottom) {
        return v;
      }
      return Value::features(v.mask & ~clearMask);
    };
  }
  return [](const Value &v) { return v; };
}

} // namespace ifds
